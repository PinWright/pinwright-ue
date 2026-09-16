// Copyright (c) 2026 Alexander Penkin. MIT License.

// Implementation of the mrq.run_jobs picture facts. Deliberately carries NO MovieRenderPipeline
// includes, exactly like its sibling MRQArtifactReport.cpp: it takes a file path and returns
// numbers, so it builds (and is tested) whether or not the engine's MovieRenderPipeline plugin is
// enabled in the target, and it can be asserted against a synthetic frame with no render at all.
#include "Handlers/MRQ/MRQFrameEvidence.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Image/ImageOps.h"
#include "Misc/Paths.h"

namespace
{
    // The suspect-reason vocabulary, spelled once. SCREAMING_SNAKE to match the plugin's error
    // codes, because these are read by the same kind of caller for the same kind of decision.
    const TCHAR* const ReasonFlatRegion = TEXT("FLAT_REGION");
    const TCHAR* const ReasonBlank      = TEXT("BLANK");
    const TCHAR* const ReasonCrushed    = TEXT("CRUSHED");
    const TCHAR* const ReasonBlownOut   = TEXT("BLOWN_OUT");

    PinWrightMRQ::FChannelImageStats MeasureChannelStats(TConstArrayView<FColor> Pixels)
    {
        PinWrightMRQ::FChannelImageStats Stats;
        if (Pixels.Num() == 0)
        {
            return Stats;
        }

        double SumRed = 0.0;
        double SumGreen = 0.0;
        double SumBlue = 0.0;
        double SumRedSquared = 0.0;
        double SumGreenSquared = 0.0;
        double SumBlueSquared = 0.0;
        for (const FColor& Pixel : Pixels)
        {
            const double Red = static_cast<double>(Pixel.R) / 255.0;
            const double Green = static_cast<double>(Pixel.G) / 255.0;
            const double Blue = static_cast<double>(Pixel.B) / 255.0;
            SumRed += Red;
            SumGreen += Green;
            SumBlue += Blue;
            SumRedSquared += Red * Red;
            SumGreenSquared += Green * Green;
            SumBlueSquared += Blue * Blue;
        }

        const double Count = static_cast<double>(Pixels.Num());
        Stats.MeanRed = SumRed / Count;
        Stats.MeanGreen = SumGreen / Count;
        Stats.MeanBlue = SumBlue / Count;
        Stats.RedVariance = FMath::Max(0.0, SumRedSquared / Count - Stats.MeanRed * Stats.MeanRed);
        Stats.GreenVariance = FMath::Max(
            0.0, SumGreenSquared / Count - Stats.MeanGreen * Stats.MeanGreen);
        Stats.BlueVariance = FMath::Max(
            0.0, SumBlueSquared / Count - Stats.MeanBlue * Stats.MeanBlue);
        Stats.bUniformColor = Stats.RedVariance <= PinWrightMRQ::UniformChannelVarianceThreshold
            && Stats.GreenVariance <= PinWrightMRQ::UniformChannelVarianceThreshold
            && Stats.BlueVariance <= PinWrightMRQ::UniformChannelVarianceThreshold;
        return Stats;
    }
}

bool PinWrightMRQ::PathIsStillImage(const FString& Path)
{
    const FString Extension = FPaths::GetExtension(Path).ToLower();
    // The formats FImageUtils::LoadImage decodes and MRQ writes. A video container is absent on
    // purpose -- see the header: it is reported as not analyzed rather than as clean.
    return Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg")
        || Extension == TEXT("bmp") || Extension == TEXT("exr") || Extension == TEXT("tga");
}

TArray<int32> PinWrightMRQ::SelectFrameSample(int32 FrameCount, int32 MaxFrames)
{
    TArray<int32> Sample;
    if (FrameCount <= 0 || MaxFrames <= 0)
    {
        return Sample;
    }
    if (FrameCount <= MaxFrames)
    {
        Sample.Reserve(FrameCount);
        for (int32 Index = 0; Index < FrameCount; ++Index)
        {
            Sample.Add(Index);
        }
        return Sample;
    }
    if (MaxFrames == 1)
    {
        // One slot goes to the first frame: an unconverged render is worst there.
        Sample.Add(0);
        return Sample;
    }
    Sample.Reserve(MaxFrames);
    for (int32 Slot = 0; Slot < MaxFrames; ++Slot)
    {
        // Spans the closed interval, so slot 0 is the first frame and the last slot is the last
        // frame however the division rounds in between.
        const int32 Ordinal = static_cast<int32>(
            (static_cast<int64>(Slot) * static_cast<int64>(FrameCount - 1)) / (MaxFrames - 1));
        Sample.AddUnique(Ordinal);
    }
    return Sample;
}

PinWrightMRQ::FFrameEvidence PinWrightMRQ::AnalyzeRenderedFrame(const FString& AbsolutePath)
{
    FFrameEvidence Evidence;

    PinWrightImage::FBitmap Bitmap;
    FString ErrCode;
    FString ErrMsg;
    if (!PinWrightImage::LoadBitmap(AbsolutePath, Bitmap, ErrCode, ErrMsg))
    {
        Evidence.NotAnalyzedReason = FString::Printf(TEXT("%s: %s"), *ErrCode, *ErrMsg);
        return Evidence;
    }

    Evidence.Width = Bitmap.Width;
    Evidence.Height = Bitmap.Height;
    // The same classifier the render.* verbs publish, over the same BGRA8 layout it expects, so
    // `blank` / `crushed` / `blownOut` mean here exactly what they mean there.
    Evidence.ImageStats = PinWrightRenderCapture::CalculateCaptureImageStats(Bitmap.Pixels);
    Evidence.ChannelStats = MeasureChannelStats(Bitmap.Pixels);
    Evidence.FlatRegion = PinWrightFlatRegion::MeasureLargestFlatRegion(
        Bitmap.Pixels, Bitmap.Width, Bitmap.Height);
    Evidence.bUnrenderable = Evidence.ChannelStats.bUniformColor;

    // Order matters only for reading: FLAT_REGION first because it is the verdict the whole-frame
    // statistics cannot reach, and therefore the one a caller is least likely to already have.
    if (Evidence.FlatRegion.bMeasured && Evidence.FlatRegion.bLargeFlatRegion)
    {
        Evidence.SuspectReasons.Add(ReasonFlatRegion);
    }
    if (Evidence.ImageStats.bBlank)
    {
        Evidence.SuspectReasons.Add(ReasonBlank);
    }
    if (Evidence.ImageStats.bCrushed)
    {
        Evidence.SuspectReasons.Add(ReasonCrushed);
    }
    if (Evidence.ImageStats.bBlownOut)
    {
        Evidence.SuspectReasons.Add(ReasonBlownOut);
    }
    Evidence.bSuspect = Evidence.SuspectReasons.Num() > 0;
    // Last, so an early return above cannot leave a partial reading looking like a measurement.
    Evidence.bAnalyzed = true;
    return Evidence;
}

void PinWrightMRQ::AddFrameEvidenceFields(const FFrameEvidence& Evidence,
    const TSharedPtr<FJsonObject>& FileEntry)
{
    if (!FileEntry.IsValid())
    {
        return;
    }
    FileEntry->SetBoolField(TEXT("imageAnalyzed"), Evidence.bAnalyzed);
    if (!Evidence.bAnalyzed)
    {
        // Nothing beyond the fact that nothing was measured. Emitting zeroed statistics here would
        // hand the caller a verdict nobody reached.
        if (!Evidence.NotAnalyzedReason.IsEmpty())
        {
            FileEntry->SetStringField(TEXT("imageNotAnalyzedReason"), Evidence.NotAnalyzedReason);
        }
        return;
    }

    TSharedPtr<FJsonObject> ImageStats = MakeShared<FJsonObject>();
    ImageStats->SetNumberField(TEXT("meanLuminance"), Evidence.ImageStats.MeanLuminance);
    ImageStats->SetNumberField(TEXT("luminanceVariance"), Evidence.ImageStats.LuminanceVariance);
    ImageStats->SetNumberField(TEXT("minLuminance"), Evidence.ImageStats.MinLuminance);
    ImageStats->SetNumberField(TEXT("maxLuminance"), Evidence.ImageStats.MaxLuminance);
    ImageStats->SetNumberField(TEXT("meanRed"), Evidence.ChannelStats.MeanRed);
    ImageStats->SetNumberField(TEXT("meanGreen"), Evidence.ChannelStats.MeanGreen);
    ImageStats->SetNumberField(TEXT("meanBlue"), Evidence.ChannelStats.MeanBlue);
    ImageStats->SetNumberField(TEXT("redVariance"), Evidence.ChannelStats.RedVariance);
    ImageStats->SetNumberField(TEXT("greenVariance"), Evidence.ChannelStats.GreenVariance);
    ImageStats->SetNumberField(TEXT("blueVariance"), Evidence.ChannelStats.BlueVariance);
    ImageStats->SetNumberField(TEXT("litPixelCount"),
        static_cast<double>(Evidence.ImageStats.LitPixelCount));
    ImageStats->SetNumberField(TEXT("litPixelFraction"), Evidence.ImageStats.LitPixelFraction);
    ImageStats->SetNumberField(TEXT("litLuminanceThreshold"),
        PinWrightRenderCapture::BlankLitLuminanceThreshold);
    // toneLevelsUsed / toneLevelMinPixels, through the same writer render.capture_* uses.
    PinWrightRenderCapture::AddToneRangeStatsFields(Evidence.ImageStats, ImageStats);
    // flatRegionFraction / flatRegionLevel / flatRegionBounds / flatBlockFraction -- the spatial
    // half, and the only fields on this block that can see a half-void frame.
    PinWrightFlatRegion::AddFlatRegionStatsFields(Evidence.FlatRegion, ImageStats);
    FileEntry->SetObjectField(TEXT("imageStats"), ImageStats);

    FileEntry->SetNumberField(TEXT("imageWidth"), Evidence.Width);
    FileEntry->SetNumberField(TEXT("imageHeight"), Evidence.Height);
    FileEntry->SetBoolField(TEXT("blank"), Evidence.ImageStats.bBlank);
    FileEntry->SetBoolField(TEXT("crushed"), Evidence.ImageStats.bCrushed);
    FileEntry->SetBoolField(TEXT("blownOut"), Evidence.ImageStats.bBlownOut);
    FileEntry->SetBoolField(TEXT("uniformColor"), Evidence.ChannelStats.bUniformColor);
    FileEntry->SetBoolField(TEXT("unrenderable"), Evidence.bUnrenderable);
    FileEntry->SetBoolField(TEXT("suspect"), Evidence.bSuspect);
    if (Evidence.SuspectReasons.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Reasons;
        Reasons.Reserve(Evidence.SuspectReasons.Num());
        for (const FString& Reason : Evidence.SuspectReasons)
        {
            Reasons.Add(MakeShared<FJsonValueString>(Reason));
        }
        FileEntry->SetArrayField(TEXT("suspectReasons"), Reasons);
    }
}

void PinWrightMRQ::AccumulateFrameEvidence(const FFrameEvidence& Evidence, const FString& Path,
    FFrameEvidenceSummary& Summary)
{
    if (!Evidence.bAnalyzed)
    {
        ++Summary.DecodeFailureCount;
        return;
    }
    ++Summary.AnalyzedCount;
    Summary.SuspectInOrder.Add(Evidence.bSuspect);
    Summary.UnrenderableCount += Evidence.bUnrenderable ? 1 : 0;
    if (!Evidence.bSuspect)
    {
        return;
    }
    ++Summary.SuspectCount;
    // The worst frame is the one with the largest flat region, which is the failure this evidence
    // was added for. A blank or collapsed frame with no flat region measured still counts as
    // suspect above; it just never wins this comparison, and its own entry carries its verdicts.
    if (Summary.WorstFramePath.IsEmpty()
        || Evidence.FlatRegion.LargestRegionFraction > Summary.WorstFlatRegionFraction)
    {
        Summary.WorstFlatRegionFraction = Evidence.FlatRegion.LargestRegionFraction;
        Summary.WorstFramePath = Path;
        Summary.WorstFrameRegion = PinWrightFlatRegion::DescribeFlatRegion(Evidence.FlatRegion);
    }
}

void PinWrightMRQ::MakeFrameEvidenceWarnings(const FFrameEvidenceSummary& Summary,
    TArray<FString>& OutWarnings)
{
    if (Summary.SuspectCount > 0)
    {
        // WHERE in the render the suspect frames fall. This is the one causal discrimination the
        // evidence can make for free, and it is the conclusion that cost the original reporter two
        // builds: the preset already carried 32 engine + 8 render warm-up frames and the void
        // reproduced anyway, which only a look at every frame could establish by hand.
        bool bAllSuspect = true;
        bool bPrefixSuspect = true;
        bool bSeenClean = false;
        for (const bool bSuspect : Summary.SuspectInOrder)
        {
            bAllSuspect = bAllSuspect && bSuspect;
            if (!bSuspect)
            {
                bSeenClean = true;
            }
            else if (bSeenClean)
            {
                // A suspect frame after a clean one: not a prefix.
                bPrefixSuspect = false;
            }
        }

        const TCHAR* ShapeNote = TEXT("");
        if (bAllSuspect)
        {
            ShapeNote = TEXT("EVERY frame that was measured is affected, so this is not a "
                "first-frame convergence artefact and raising the engine/render warm-up counts "
                "will not fix it on its own.");
        }
        else if (bPrefixSuspect)
        {
            ShapeNote = TEXT("Only the earliest measured frames are affected and everything after "
                "them is clean, which is the shape of a render that had not converged when it "
                "started writing -- more engine/render warm-up frames are the first thing to try.");
        }
        else
        {
            ShapeNote = TEXT("The affected frames are scattered through the render rather than "
                "confined to its start, so warm-up is not the whole mechanism.");
        }

        OutWarnings.Add(FString::Printf(
            TEXT("%d of the %d rendered frame(s) opened by this result are SUSPECT: the worst is ")
            TEXT("%s, whose largest connected low-variance region is %s. %s This is a ")
            TEXT("measurement of the ")
            TEXT("PIXELS, and it is the only one here that is: `jobSucceeded`, `fileSizeBytes`, ")
            TEXT("`totalFileSizeBytes` and `bitsPerPixel` are all properties of the FILE and stay ")
            TEXT("healthy on a frame whose lower half is a shallow low-variance void, because a ")
            TEXT("large smooth region compresses well while the pixel count keeps the byte count ")
            TEXT("plausible. ")
            TEXT("Open that file before shipping the render. Causes this verb cannot tell apart, ")
            TEXT("in the order they are worth checking: Nanite / virtual-shadow-map / texture ")
            TEXT("streaming that had not converged when the frame was written; geometry that was ")
            TEXT("never loaded into the PIE world at all (check the level the job was queued ")
            TEXT("against, and `shots[].state` on this job); and an accumulation that was reset ")
            TEXT("mid-frame by a GPU timeout. A deliberately flat backdrop lands here too and is ")
            TEXT("not an error -- `imageStats.flatRegionBounds` says where the region is so a ")
            TEXT("caller can tell a sky from a hole."),
            Summary.SuspectCount,
            Summary.AnalyzedCount,
            *Summary.WorstFramePath,
            Summary.WorstFrameRegion.IsEmpty()
                ? TEXT("not measurable at this frame size")
                : *Summary.WorstFrameRegion,
            ShapeNote));
    }

    if (Summary.AnalyzedCount < Summary.StillImageCount)
    {
        // Said whether or not anything was suspect. A clean sample of a long render is a statement
        // about the sample, and reading it as a statement about the render is precisely the
        // false-confidence this whole ticket is about.
        OutWarnings.Add(FString::Printf(
            TEXT("Only %d of the %d still frames this render wrote were decoded and measured ")
            TEXT("(evenly spaced, first and last always included) — opening every frame of a long ")
            TEXT("render costs more game-thread time than the signal is worth. So ")
            TEXT("`framesSuspect` describes THE SAMPLE, not the render: a void confined to frames ")
            TEXT("nobody opened is not reported here. Run image.compare or open the frames ")
            TEXT("yourself if the render has to be certified frame by frame."),
            Summary.AnalyzedCount, Summary.StillImageCount));
    }

    if (Summary.DecodeFailureCount > 0)
    {
        OutWarnings.Add(FString::Printf(
            TEXT("%d file(s) the render reported writing could not be decoded, so nothing about ")
            TEXT("their pixels was measured and they are neither counted as clean nor as suspect ")
            TEXT("(each carries `imageAnalyzed: false` and the decoder's own message). A file ")
            TEXT("that stat'd but will not decode is usually still being written or was written "
                 "truncated."),
            Summary.DecodeFailureCount));
    }
}
