// Copyright (c) 2026 Alexander Penkin. MIT License.

// Implementation of the mrq.run_jobs artifact report and the mrq.create_job pre-flight disclosure.
// Deliberately carries NO MovieRenderPipeline includes: both take plain values plus an
// already-extracted context, so they build (and are tested) whether or not the engine's
// MovieRenderPipeline plugin is enabled in the target.
#include "Handlers/MRQ/MRQArtifactReport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
// The picture half of the same report: decode the frames this render wrote and measure them, so
// the block below is not made entirely of file facts. See MRQFrameEvidence.h for why the size of
// a PNG cannot see a half-void frame.
#include "Handlers/MRQ/MRQFrameEvidence.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

namespace
{
    const FProperty* FindSettingProperty(const UObject* Object, const TCHAR* PropertyName)
    {
        return Object ? Object->GetClass()->FindPropertyByName(FName(PropertyName)) : nullptr;
    }

    // Numeric read that accepts whichever width the engine declared the knob at — the MP4 encoder
    // spells its bitrates `float` and its CRF `int32`, and a sibling video output could differ.
    bool TryReadNumberProperty(const UObject* Object, const TCHAR* PropertyName, double& OutValue)
    {
        const FProperty* Property = FindSettingProperty(Object, PropertyName);
        if (!Property)
        {
            return false;
        }
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
        {
            OutValue = FloatProperty->GetPropertyValue(ValuePtr);
            return true;
        }
        if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
        {
            OutValue = DoubleProperty->GetPropertyValue(ValuePtr);
            return true;
        }
        if (const FIntProperty* IntProperty = CastField<FIntProperty>(Property))
        {
            OutValue = static_cast<double>(IntProperty->GetPropertyValue(ValuePtr));
            return true;
        }
        return false;
    }

    // A UENUM property reaches reflection as either FEnumProperty (enum class) or FByteProperty
    // with an Enum attached (TEnumAsByte); both shapes are read here so the readback does not
    // depend on how a particular output class happened to declare its mode.
    bool TryReadEnumPropertyName(const UObject* Object, const TCHAR* PropertyName, FString& OutValue)
    {
        const FProperty* Property = FindSettingProperty(Object, PropertyName);
        if (!Property)
        {
            return false;
        }
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        const UEnum* Enum = nullptr;
        int64 RawValue = 0;
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            Enum = EnumProperty->GetEnum();
            RawValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
        }
        else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            Enum = ByteProperty->Enum;
            RawValue = static_cast<int64>(ByteProperty->GetPropertyValue(ValuePtr));
        }
        if (!Enum)
        {
            return false;
        }
        // GetNameStringByValue returns the short enumerator name ("Quality"), empty for a value
        // the enum does not carry — which is omitted rather than published as a number.
        OutValue = Enum->GetNameStringByValue(RawValue);
        return !OutValue.IsEmpty();
    }
}

TSharedPtr<FJsonObject> PinWrightMRQ::ReadRequestedEncoderSettings(const UObject* VideoOutputSetting)
{
    if (!VideoOutputSetting)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Encoder = MakeShared<FJsonObject>();
    Encoder->SetStringField(TEXT("class"), VideoOutputSetting->GetClass()->GetPathName());

    FString RateControl;
    if (TryReadEnumPropertyName(VideoOutputSetting, TEXT("EncodingRateControl"), RateControl))
    {
        Encoder->SetStringField(TEXT("rateControl"), RateControl);
    }
    double Number = 0.0;
    if (TryReadNumberProperty(VideoOutputSetting, TEXT("ConstantRateFactor"), Number))
    {
        Encoder->SetNumberField(TEXT("constantRateFactor"), Number);
    }
    if (TryReadNumberProperty(VideoOutputSetting, TEXT("AverageBitrateInMbps"), Number))
    {
        Encoder->SetNumberField(TEXT("averageBitrateMbps"), Number);
    }
    if (TryReadNumberProperty(VideoOutputSetting, TEXT("MaxBitrateInMbps"), Number))
    {
        Encoder->SetNumberField(TEXT("maxBitrateMbps"), Number);
    }
    return Encoder;
}

bool PinWrightMRQ::RateControlIsUnboundedBelow(const FString& RateControlName)
{
    return RateControlName.Equals(TEXT("Quality"), ESearchCase::IgnoreCase)
        || RateControlName.Equals(TEXT("ConstantQP"), ESearchCase::IgnoreCase);
}

TSharedPtr<FJsonObject> PinWrightMRQ::BuildArtifactReport(const TArray<FRenderedFile>& Files,
    const FEncodeContext& Context)
{
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Warnings;

    IFileManager& FileManager = IFileManager::Get();
    TArray<TSharedPtr<FJsonValue>> FileEntries;
    FileEntries.Reserve(Files.Num());
    int64 TotalSizeBytes = 0;
    int32 MeasuredFiles = 0;
    int32 MissingFiles = 0;

    // Which of the reported files are frames at all, and which of THOSE will be opened. Decided
    // before the loop because the sample has to be spread over the whole sequence -- the first
    // frame and the last one are the two that matter most, and neither is knowable one file at a
    // time (MRQFrameEvidence.h).
    FFrameEvidenceSummary FrameSummary;
    TArray<int32> StillImageOrdinalOfFile;
    StillImageOrdinalOfFile.Init(INDEX_NONE, Files.Num());
    for (int32 Index = 0; Index < Files.Num(); ++Index)
    {
        if (PathIsStillImage(Files[Index].Path))
        {
            StillImageOrdinalOfFile[Index] = FrameSummary.StillImageCount++;
        }
    }
    const TArray<int32> SampledOrdinals =
        SelectFrameSample(FrameSummary.StillImageCount, MaxAnalyzedFramesPerJob);

    for (int32 Index = 0; Index < Files.Num(); ++Index)
    {
        const FRenderedFile& File = Files[Index];
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), File.Path);
        if (!File.RenderPass.IsEmpty())
        {
            Entry->SetStringField(TEXT("renderPass"), File.RenderPass);
        }

        const FFileStatData Stat = FileManager.GetStatData(*File.Path);
        const bool bMeasured = Stat.bIsValid && !Stat.bIsDirectory && Stat.FileSize >= 0;
        Entry->SetBoolField(TEXT("exists"), bMeasured);
        if (bMeasured)
        {
            // Measured, so it is named plainly. Absent below rather than 0: a zero size would be
            // indistinguishable from a real measurement of an empty file.
            Entry->SetNumberField(TEXT("fileSizeBytes"), static_cast<double>(Stat.FileSize));
            TotalSizeBytes += Stat.FileSize;
            ++MeasuredFiles;
        }
        else
        {
            ++MissingFiles;
        }

        // Every entry says whether its pixels were looked at, including the ones that were not:
        // an absent `imageAnalyzed` would read as "measured and fine", which is the shape of
        // silent success this whole block exists to remove.
        //
        // The three not-analyzed reasons are CONSTANT strings rather than per-entry Printfs. A
        // long render produces hundreds of entries, and a sentence formatted into each of them is
        // response weight paid per frame for a fact that is identical on all of them; the
        // per-render detail lives once, in the job's sampling warning.
        const int32 StillOrdinal = StillImageOrdinalOfFile[Index];
        if (StillOrdinal == INDEX_NONE)
        {
            FFrameEvidence NotAFrame;
            NotAFrame.NotAnalyzedReason =
                TEXT("Not a still-image file this verb can decode, so nothing about its pixels ")
                TEXT("was measured. For a video container the size and bitrate fields are the ")
                TEXT("only picture-adjacent signal available here.");
            AddFrameEvidenceFields(NotAFrame, Entry);
        }
        else if (!bMeasured)
        {
            FFrameEvidence NotOnDisk;
            NotOnDisk.NotAnalyzedReason =
                TEXT("The file was not on disk when this result was built, so there was nothing to ")
                TEXT("decode. See `exists` above.");
            AddFrameEvidenceFields(NotOnDisk, Entry);
            if (SampledOrdinals.Contains(StillOrdinal))
            {
                AccumulateFrameEvidence(NotOnDisk, File.Path, FrameSummary);
            }
        }
        else if (!SampledOrdinals.Contains(StillOrdinal))
        {
            FFrameEvidence NotSampled;
            NotSampled.NotAnalyzedReason =
                TEXT("Not in the sampled subset; see this job's sampling warning for how many ")
                TEXT("frames were opened.");
            AddFrameEvidenceFields(NotSampled, Entry);
        }
        else
        {
            const FFrameEvidence Evidence = AnalyzeRenderedFrame(File.Path);
            AddFrameEvidenceFields(Evidence, Entry);
            AccumulateFrameEvidence(Evidence, File.Path, FrameSummary);
        }

        FileEntries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    Report->SetArrayField(TEXT("outputFiles"), FileEntries);
    Report->SetNumberField(TEXT("outputFileCount"), Files.Num());
    Report->SetNumberField(TEXT("measuredFileCount"), MeasuredFiles);
    // Published even when they are zero: "no frame was opened" is a fact a caller must be able to
    // read off the response, and it is the difference between a clean render and an unchecked one.
    Report->SetNumberField(TEXT("framesAnalyzed"), FrameSummary.AnalyzedCount);
    Report->SetNumberField(TEXT("framesSuspect"), FrameSummary.SuspectCount);
    Report->SetNumberField(TEXT("framesUnrenderable"), FrameSummary.UnrenderableCount);
    Report->SetBoolField(TEXT("allAnalyzedFramesUnrenderable"),
        FrameSummary.AnalyzedCount > 0 && FrameSummary.DecodeFailureCount == 0
            && FrameSummary.UnrenderableCount == FrameSummary.AnalyzedCount);
    if (MeasuredFiles > 0)
    {
        Report->SetNumberField(TEXT("totalFileSizeBytes"), static_cast<double>(TotalSizeBytes));
    }

    // Ahead of the size/bitrate warnings on purpose: an unusable picture outranks an implausible
    // byte count, and the first warning a caller reads should be the one that stops the ship.
    {
        TArray<FString> FrameWarnings;
        MakeFrameEvidenceWarnings(FrameSummary, FrameWarnings);
        for (const FString& FrameWarning : FrameWarnings)
        {
            Warnings.Add(MakeShared<FJsonValueString>(FrameWarning));
        }
    }

    if (Files.Num() == 0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("The render reported no output files, so NOTHING about the artifact was measured: ")
            TEXT("totalFileSizeBytes and overallBitrateBps are omitted rather than reported as 0. ")
            TEXT("A job that wrote files but reports none here means the pipeline produced no ")
            TEXT("output data for it — check that the preset carries an enabled output setting.")));
    }
    else if (MissingFiles > 0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%d of %d files the render reported writing were not on disk when this result was ")
            TEXT("built. fileSizeBytes is omitted for those rather than reported as 0, and ")
            TEXT("totalFileSizeBytes/overallBitrateBps below describe only the %d that were ")
            TEXT("measured."),
            MissingFiles, Files.Num(), MeasuredFiles)));
    }

    // Duration is derived from the pipeline's own output-frame count and frame rate, not demuxed
    // from the file. Published only when both halves were reported.
    TOptional<double> DurationSeconds;
    if (Context.FrameCount.IsSet())
    {
        Report->SetNumberField(TEXT("frameCount"), Context.FrameCount.GetValue());
    }
    if (Context.FrameRate.IsSet())
    {
        Report->SetNumberField(TEXT("frameRate"), Context.FrameRate.GetValue());
    }
    if (Context.FrameCount.IsSet() && Context.FrameRate.IsSet()
        && Context.FrameCount.GetValue() > 0 && Context.FrameRate.GetValue() > 0.0)
    {
        DurationSeconds = static_cast<double>(Context.FrameCount.GetValue()) / Context.FrameRate.GetValue();
        Report->SetNumberField(TEXT("durationSeconds"), DurationSeconds.GetValue());
    }

    if (Context.Width.IsSet() && Context.Height.IsSet())
    {
        TSharedPtr<FJsonObject> Resolution = MakeShared<FJsonObject>();
        Resolution->SetNumberField(TEXT("width"), Context.Width.GetValue());
        Resolution->SetNumberField(TEXT("height"), Context.Height.GetValue());
        Report->SetObjectField(TEXT("resolution"), Resolution);
    }

    TOptional<double> BitrateBps;
    if (MeasuredFiles > 0 && TotalSizeBytes == 0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("Every file the render reported writing is 0 bytes on disk. overallBitrateBps is ")
            TEXT("omitted rather than reported as 0 — there is nothing here to measure a bitrate ")
            TEXT("of, and the artifact is not usable.")));
    }
    else if (MeasuredFiles > 0 && DurationSeconds.IsSet())
    {
        BitrateBps = static_cast<double>(TotalSizeBytes) * 8.0 / DurationSeconds.GetValue();
        Report->SetNumberField(TEXT("overallBitrateBps"), BitrateBps.GetValue());
    }
    else if (MeasuredFiles > 0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("The output size was measured but the render reported no output frame count and ")
            TEXT("frame rate, so no duration could be derived and overallBitrateBps is OMITTED ")
            TEXT("rather than reported as 0. Bitrate is the number that separates a good encode ")
            TEXT("from a banded one; divide totalFileSizeBytes*8 by the clip's duration yourself ")
            TEXT("and check it against ~0.04 bits/pixel/frame.")));
    }

    TOptional<double> BitsPerPixel;
    if (BitrateBps.IsSet() && Context.Width.IsSet() && Context.Height.IsSet()
        && Context.FrameRate.IsSet()
        && Context.Width.GetValue() > 0 && Context.Height.GetValue() > 0
        && Context.FrameRate.GetValue() > 0.0)
    {
        const double PixelsPerSecond = static_cast<double>(Context.Width.GetValue())
            * static_cast<double>(Context.Height.GetValue()) * Context.FrameRate.GetValue();
        BitsPerPixel = BitrateBps.GetValue() / PixelsPerSecond;
        Report->SetNumberField(TEXT("bitsPerPixel"), BitsPerPixel.GetValue());
    }

    FString RateControl;
    if (Context.RequestedEncoder.IsValid())
    {
        // Named `encoderRequested`, never `encoder`: these are the values the config ASKED the
        // encoder for. On the Quality path they do not bound the achieved bitrate at all.
        Report->SetObjectField(TEXT("encoderRequested"), Context.RequestedEncoder);
        Context.RequestedEncoder->TryGetStringField(TEXT("rateControl"), RateControl);
    }

    if (BitsPerPixel.IsSet() && BitsPerPixel.GetValue() < MinPlausibleBitsPerPixel)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Encoded at %.4g bits/pixel (%.2f Mbps at %dx%d@%.4g) — %.1fx below the %.2f ")
            TEXT("bits/pixel plausibility floor. A file this small for this many pixels is the ")
            TEXT("signature of quality-targeted encoding on low-detail content (fog, night, ")
            TEXT("smoke, underwater, flat gradients), which bands visibly while every other ")
            TEXT("reported number stays correct. Look at the file before shipping it."),
            BitsPerPixel.GetValue(),
            BitrateBps.GetValue() / 1000000.0,
            Context.Width.GetValue(), Context.Height.GetValue(), Context.FrameRate.GetValue(),
            MinPlausibleBitsPerPixel / BitsPerPixel.GetValue(),
            MinPlausibleBitsPerPixel)));
    }

    if (RateControlIsUnboundedBelow(RateControl))
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Rate control is '%s', which targets a quality and has NO lower bitrate bound: ")
            TEXT("the Media Foundation writer's quality branch sets an encode QP and never sets ")
            TEXT("a mean or max bitrate, so a low-detail shot can encode arbitrarily small. This ")
            TEXT("is the engine's shipped default (Quality, CRF 20). Set the video output ")
            TEXT("setting to VariableBitRate with an explicit AverageBitrateInMbps if the shot ")
            TEXT("is fog, night, smoke, underwater or otherwise mostly smooth gradients."),
            *RateControl)));
    }

    // Emitted only when non-empty, matching the plugin's dominant `warnings` convention.
    if (Warnings.Num() > 0)
    {
        Report->SetArrayField(TEXT("warnings"), Warnings);
    }
    return Report;
}

TSharedPtr<FJsonObject> PinWrightMRQ::BuildPreflightReport(const FPreflightContext& Context,
    TArray<FString>& OutWarnings)
{
    TSharedPtr<FJsonObject> Preflight = MakeShared<FJsonObject>();

    if (Context.Width.IsSet() && Context.Height.IsSet())
    {
        TSharedPtr<FJsonObject> Resolution = MakeShared<FJsonObject>();
        Resolution->SetNumberField(TEXT("width"), Context.Width.GetValue());
        Resolution->SetNumberField(TEXT("height"), Context.Height.GetValue());
        Preflight->SetObjectField(TEXT("resolution"), Resolution);
    }
    // Both are format strings the pipeline expands at render time; they are published under the
    // engine's own property names so a caller can go edit the setting they name.
    if (!Context.OutputDirectory.IsEmpty())
    {
        Preflight->SetStringField(TEXT("outputDirectory"), Context.OutputDirectory);
    }
    if (!Context.FileNameFormat.IsEmpty())
    {
        Preflight->SetStringField(TEXT("fileNameFormat"), Context.FileNameFormat);
    }
    // Omitted, not defaulted to the sequence rate: the sequence's rate is not readable here, and
    // publishing a guess under a measurement name is the defect this whole ticket is about.
    if (Context.FrameRateOverride.IsSet())
    {
        Preflight->SetNumberField(TEXT("frameRateOverride"), Context.FrameRateOverride.GetValue());
    }

    TArray<TSharedPtr<FJsonValue>> OutputClasses;
    OutputClasses.Reserve(Context.OutputClassPaths.Num());
    for (const FString& ClassPath : Context.OutputClassPaths)
    {
        OutputClasses.Add(MakeShared<FJsonValueString>(ClassPath));
    }
    Preflight->SetArrayField(TEXT("outputs"), OutputClasses);

    FString RateControl;
    if (Context.RequestedEncoder.IsValid())
    {
        Preflight->SetObjectField(TEXT("encoderRequested"), Context.RequestedEncoder);
        Context.RequestedEncoder->TryGetStringField(TEXT("rateControl"), RateControl);
    }

    if (Context.OutputClassPaths.Num() == 0)
    {
        OutWarnings.Add(
            TEXT("The queued job's resolved configuration carries NO enabled output setting, so ")
            TEXT("this render will write no files at all. mrq.run_jobs will still report ")
            TEXT("success:true for it. Queue the job with a presetPath whose primary config ")
            TEXT("carries an output (mrq.list_presets enumerates them), or add one to the preset ")
            TEXT("asset before rendering."));
    }
    else if (!Context.RequestedEncoder.IsValid())
    {
        OutWarnings.Add(FString::Printf(
            TEXT("No video output setting is enabled on the resolved configuration, so this job ")
            TEXT("writes an image sequence (%s) rather than a video file, and no encoder or ")
            TEXT("rate-control settings could be disclosed. Add a video output to the preset if a ")
            TEXT("movie file was expected."),
            *FString::Join(Context.OutputClassPaths, TEXT(", "))));
    }

    if (RateControlIsUnboundedBelow(RateControl))
    {
        OutWarnings.Add(FString::Printf(
            TEXT("Rate control is '%s' BEFORE the render starts: it targets a quality and has no ")
            TEXT("lower bitrate bound, because the Media Foundation writer's quality branch sets ")
            TEXT("an encode QP and never a mean or max bitrate. This is the engine's shipped ")
            TEXT("default (Quality, CRF 20) and it bands visibly on fog, night, smoke, underwater ")
            TEXT("and other smooth-gradient shots while every other number the render reports ")
            TEXT("stays correct. Set the video output setting to VariableBitRate with an explicit ")
            TEXT("AverageBitrateInMbps now, rather than after spending the render — a shot like ")
            TEXT("that wants at least %.2f bits/pixel/frame, which mrq.run_jobs checks the ")
            TEXT("finished file against."),
            *RateControl, MinPlausibleBitsPerPixel));
    }

    return Preflight;
}
