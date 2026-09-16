// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/ThumbnailFrameEvidence.h"

#include "Compat/EngineVersionCompat.h"

#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "RHI.h"
#include "ShaderCore.h"

namespace PinWrightThumbnail
{
    void WaitForThumbnailSubjectReadiness(UObject* Asset, FThumbnailReadinessReport& OutReport)
    {
        OutReport = FThumbnailReadinessReport();
        if (!Asset)
        {
            return;
        }

        // The wait set: the subject, plus -- when the subject is a material -- every texture it
        // samples. A material's own async work being finished says nothing about the mips of the
        // textures its shader reads, and unstreamed mips are what the washed-out, semi-transparent
        // grey first frame in B-thumbnail-cold-first-frame-no-stats looks like.
        TArray<UObject*> WaitSet;
        WaitSet.Add(Asset);

        TArray<UTexture*> SubjectTextures;
        if (UTexture* AsTexture = Cast<UTexture>(Asset))
        {
            SubjectTextures.Add(AsTexture);
        }
        else if (UMaterialInterface* AsMaterial = Cast<UMaterialInterface>(Asset))
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            // The ERHIFeatureLevel overloads were made `final { return NULL; }` and deprecated in
            // 5.7 in favour of the EShaderPlatform ones; calling them here would silently collect
            // nothing.
            AsMaterial->GetUsedTextures(SubjectTextures);
#else
            AsMaterial->GetUsedTextures(SubjectTextures, EMaterialQualityLevel::Num,
                /*bAllQualityLevels*/ true, GMaxRHIFeatureLevel, /*bAllFeatureLevels*/ true);
#endif
            for (UTexture* Texture : SubjectTextures)
            {
                if (Texture)
                {
                    WaitSet.Add(Texture);
                }
            }
        }

        // The targeted form of what EThumbnailTextureFlushMode::AlwaysFlush does globally. Scoped
        // to this subject so a thumbnail of one asset does not block on every outstanding build in
        // the editor; it still falls back to FinishAllCompilation on its own if an object reports
        // itself as still compiling afterwards (AssetCompilingManager.cpp).
        FAssetCompilingManager::Get().FinishCompilationForObjects(WaitSet);
        OutReport.bWaitedForAssetCompilation = true;

        // Compilation finishing is not the same question as mips being resident, and only the
        // second one decides whether the thumbnail shows the texture or a blurry stand-in. This is
        // the pre-wait ObjectTools::GenerateThumbnailForObjectToSaveToDisk does for a UTexture
        // subject, applied to every texture the frame will actually sample.
        for (UTexture* Texture : SubjectTextures)
        {
            if (!Texture)
            {
                continue;
            }
            Texture->WaitForStreaming();
            ++OutReport.TexturesWaitedFor;
        }
        OutReport.bWaitedForTextureStreaming = OutReport.TexturesWaitedFor > 0;

        if (UMaterialInterface* AsMaterial = Cast<UMaterialInterface>(Asset))
        {
            // The engine's own thumbnail path calls this out as required specifically for
            // thumbnails, and it is a DIFFERENT question from the one material.authoring.
            // compile_material answers: that verb blocks on GShaderCompilingManager->
            // FinishAllCompilation(), which is about the compile queue draining, while this asks
            // whether THIS resource's game-thread shader map is complete. A material can pass the
            // first and fail the second, which is why a fixer aiming at compilation state finds
            // nothing to fix here.
            //
            // Widened from the engine's UMaterial-only branch to UMaterialInterface: a material
            // instance has its own FMaterialResource and its own shader map, and it is exactly as
            // able to be drawn before that map is complete.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            FMaterialResource* Resource = AsMaterial->GetMaterialResource(GMaxRHIShaderPlatform);
#else
            FMaterialResource* Resource = AsMaterial->GetMaterialResource(GMaxRHIFeatureLevel);
#endif
            if (Resource)
            {
                OutReport.bShaderMapChecked = true;
                OutReport.bShaderMapCompleteBefore = Resource->IsGameThreadShaderMapComplete();
                if (!OutReport.bShaderMapCompleteBefore)
                {
                    Resource->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
                }
                Resource->FinishCompilation();
                // Read back AFTER the wait rather than assuming it worked. A `false` -> `true`
                // pair in the response is the direct evidence that this call would have rendered
                // a cold frame without the wait; a `false` -> `false` pair says the wait did not
                // achieve completeness and the frame below should be read with that in mind.
                OutReport.bShaderMapCompleteAfter = Resource->IsGameThreadShaderMapComplete();
            }
        }
    }

    TArray<FColor> ThumbnailBytesToColors(TConstArrayView<uint8> Bgra8)
    {
        TArray<FColor> ColorData;
        ColorData.Reserve(Bgra8.Num() / 4);
        for (int32 Index = 0; Index + 3 < Bgra8.Num(); Index += 4)
        {
            FColor Color;
            Color.B = Bgra8[Index + 0];
            Color.G = Bgra8[Index + 1];
            Color.R = Bgra8[Index + 2];
            Color.A = Bgra8[Index + 3];
            ColorData.Add(Color);
        }
        return ColorData;
    }

    bool FrameIsDegenerate(const PinWrightRenderCapture::FCaptureImageStats& Stats)
    {
        if (!Stats.bStatsMeasured)
        {
            // No pixels were looked at, which is the strongest cold-frame signal there is. Note
            // this is NOT the same as reading ToneLevelsUsed off an unmeasured struct: that field
            // defaults to 0 and would read as the most collapsed frame possible whether or not
            // anything was measured (rpc-design.md section 4).
            return true;
        }
        return Stats.bBlank ||
            Stats.ToneLevelsUsed < PinWrightRenderCapture::MinUsableToneLevels;
    }

    int64 CountDifferingPixels(TConstArrayView<FColor> First, TConstArrayView<FColor> Second)
    {
        const int32 Common = FMath::Min(First.Num(), Second.Num());
        int64 Differing = 0;
        for (int32 Index = 0; Index < Common; ++Index)
        {
            Differing += (First[Index] != Second[Index]) ? 1 : 0;
        }
        return Differing;
    }

    FString MakeThumbnailFrameWarning(const PinWrightRenderCapture::FCaptureImageStats& Stats,
        int32 RenderPasses)
    {
        if (!Stats.bStatsMeasured)
        {
            return TEXT("No pixels were measured for this thumbnail: the render returned an empty ")
                   TEXT("image buffer. Nothing in `imageStats` was computed, so treat the reported ")
                   TEXT("size as the request rather than as a description of an image.");
        }
        if (!FrameIsDegenerate(Stats))
        {
            return FString();
        }
        return FString::Printf(
            TEXT("This thumbnail carries no readable image: %s, it resolves only %d of 256 ")
            TEXT("luminance levels (fewer than %d), at mean luminance %.6f and variance %.8f, so ")
            TEXT("it cannot show a gradient, a shadow terminator or a material response whatever ")
            TEXT("was rendered into it. On this verb the usual cause is a COLD FRAME: the first ")
            TEXT("render after a compile or a fresh load comes back blank or half-drawn while an ")
            TEXT("identical second call returns the finished picture. This call rendered %d pass(es) ")
            TEXT("and waited on the subject's compilation, textures and shader map first (see ")
            TEXT("`readiness`), so a frame still degenerate here is more likely to be the asset ")
            TEXT("than the warm-up -- a genuinely black or flat-coloured asset lands here too and ")
            TEXT("is not an error. Do NOT compare this frame against another capture: a cold frame ")
            TEXT("differs from a warm one over almost the whole image, and that difference is ")
            TEXT("warm-up artefact, not a change in the subject. A level had to carry %lld pixels ")
            TEXT("to count as populated at this image size."),
            Stats.bBlank ? TEXT("nothing was drawn into it") : TEXT("what was drawn cannot be read"),
            Stats.ToneLevelsUsed,
            PinWrightRenderCapture::MinUsableToneLevels,
            Stats.MeanLuminance,
            Stats.LuminanceVariance,
            RenderPasses,
            static_cast<long long>(Stats.ToneLevelMinPixelsUsed));
    }

    void AddFrameEvidenceFields(
        const PinWrightRenderCapture::FCaptureImageStats& Stats,
        const FColdFrameRetryReport& ColdRetry,
        int32 RenderPasses,
        const FThumbnailReadinessReport& Readiness,
        const TSharedPtr<FJsonObject>& Result)
    {
        if (!Result.IsValid())
        {
            return;
        }

        Result->SetNumberField(TEXT("renderPasses"), RenderPasses);

        TSharedPtr<FJsonObject> ReadinessObject = MakeShared<FJsonObject>();
        ReadinessObject->SetBoolField(TEXT("assetCompilationWaited"),
            Readiness.bWaitedForAssetCompilation);
        ReadinessObject->SetBoolField(TEXT("textureStreamingWaited"),
            Readiness.bWaitedForTextureStreaming);
        ReadinessObject->SetNumberField(TEXT("texturesWaitedFor"), Readiness.TexturesWaitedFor);
        ReadinessObject->SetBoolField(TEXT("shaderMapChecked"), Readiness.bShaderMapChecked);
        if (Readiness.bShaderMapChecked)
        {
            // Omitted rather than defaulted when the subject has no material resource: a
            // `shaderMapCompleteBefore: false` on a static mesh would read as a measurement of a
            // question nobody asked.
            ReadinessObject->SetBoolField(TEXT("shaderMapCompleteBefore"),
                Readiness.bShaderMapCompleteBefore);
            ReadinessObject->SetBoolField(TEXT("shaderMapCompleteAfter"),
                Readiness.bShaderMapCompleteAfter);
        }
        Result->SetObjectField(TEXT("readiness"), ReadinessObject);

        if (!Stats.bStatsMeasured)
        {
            // Nothing was measured, so nothing is published beyond the fact that nothing was
            // measured. Emitting zeroed statistics here would hand the caller the most collapsed
            // frame possible as if it had been observed.
            Result->SetBoolField(TEXT("imageStatsMeasured"), false);
            Result->SetStringField(TEXT("frameWarning"),
                MakeThumbnailFrameWarning(Stats, RenderPasses));
            return;
        }

        Result->SetBoolField(TEXT("imageStatsMeasured"), true);

        TSharedPtr<FJsonObject> ImageStats = MakeShared<FJsonObject>();
        ImageStats->SetNumberField(TEXT("meanLuminance"), Stats.MeanLuminance);
        ImageStats->SetNumberField(TEXT("luminanceVariance"), Stats.LuminanceVariance);
        ImageStats->SetNumberField(TEXT("minLuminance"), Stats.MinLuminance);
        ImageStats->SetNumberField(TEXT("maxLuminance"), Stats.MaxLuminance);
        ImageStats->SetNumberField(TEXT("litPixelCount"), static_cast<double>(Stats.LitPixelCount));
        ImageStats->SetNumberField(TEXT("litPixelFraction"), Stats.LitPixelFraction);
        ImageStats->SetNumberField(TEXT("litLuminanceThreshold"),
            PinWrightRenderCapture::BlankLitLuminanceThreshold);
        // toneLevelsUsed / toneLevelMinPixels, through the same writer the render.* verbs use, so
        // the two families cannot drift apart on what the number means.
        PinWrightRenderCapture::AddToneRangeStatsFields(Stats, ImageStats);
        Result->SetObjectField(TEXT("imageStats"), ImageStats);

        // Top-level, at the same prominence the render.* verbs give them, and REPORTED rather than
        // rejected: a flat or black asset is a legitimate thumbnail and the caller keeps the file
        // either way. `blank` is "nothing was drawn"; `crushed` / `blownOut` are "what was drawn
        // cannot be read", which is the half a blank test structurally cannot see.
        Result->SetBoolField(TEXT("blank"), Stats.bBlank);
        Result->SetBoolField(TEXT("crushed"), Stats.bCrushed);
        Result->SetBoolField(TEXT("blownOut"), Stats.bBlownOut);

        const FString FrameWarning = MakeThumbnailFrameWarning(Stats, RenderPasses);
        if (!FrameWarning.IsEmpty())
        {
            Result->SetStringField(TEXT("frameWarning"), FrameWarning);
        }

        if (ColdRetry.bRetried)
        {
            // What the first pass measured and how much of the image changed when it was drawn
            // again with identical inputs. A large differing fraction here is the measurement a
            // caller needs before believing ANY comparison against another capture: it says this
            // subject's first frame was still settling, so a t0/t1 pair spanning it measures the
            // warm-up and not the subject.
            TSharedPtr<FJsonObject> RetryObject = MakeShared<FJsonObject>();
            RetryObject->SetBoolField(TEXT("firstPassBlank"), ColdRetry.FirstPassStats.bBlank);
            RetryObject->SetBoolField(TEXT("firstPassMeasured"),
                ColdRetry.FirstPassStats.bStatsMeasured);
            if (ColdRetry.FirstPassStats.bStatsMeasured)
            {
                RetryObject->SetNumberField(TEXT("firstPassToneLevelsUsed"),
                    ColdRetry.FirstPassStats.ToneLevelsUsed);
                RetryObject->SetNumberField(TEXT("firstPassMeanLuminance"),
                    ColdRetry.FirstPassStats.MeanLuminance);
            }
            RetryObject->SetNumberField(TEXT("differingPixels"),
                static_cast<double>(ColdRetry.DifferingPixels));
            RetryObject->SetNumberField(TEXT("comparedPixels"),
                static_cast<double>(ColdRetry.ComparedPixels));
            RetryObject->SetNumberField(TEXT("differingFraction"),
                ColdRetry.ComparedPixels > 0
                    ? static_cast<double>(ColdRetry.DifferingPixels) /
                          static_cast<double>(ColdRetry.ComparedPixels)
                    : 0.0);
            RetryObject->SetBoolField(TEXT("frameChanged"), ColdRetry.DifferingPixels > 0);
            Result->SetObjectField(TEXT("coldFrameRetry"), RetryObject);
        }
    }
}
