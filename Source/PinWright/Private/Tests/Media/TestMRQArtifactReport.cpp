// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the mrq.run_jobs artifact report — the block that turns a bare `{"success":true}`
// into a statement about the file the render produced.
//
// THE DEFECT THESE PIN. mrq.run_jobs resolved its ticket with one boolean and published nothing
// about the artifact. A 20 s 1080p60 cinematic shipped at 1.16 Mbps / 3.4 MB with heavy banding
// while resolution, frame count, duration and container magic all verified correct against the
// finished file — the only number that separated it from its 21.24 Mbps re-render, bits per
// second, was not reported by any verb (B-mrq-render-result-omits-bitrate-and-size).
//
// WHY THESE TESTS AND NOT AN END-TO-END RENDER. A real MRQ render needs a PIE session, a saved
// map and a level sequence, and runs for minutes; it is not runnable inside the automation suite,
// so the terminal-result assembly in MRQHandler.cpp is not driven here. What IS driven is the
// function that produces every new field — against REAL files on disk, written by the test with
// known byte counts, and against the REAL engine encoder class read through the shipped
// reflection path. Faking a render to reach the handler would prove less than measuring a real
// file does.
//
// COUNTERFACTUALS, one per test:
//  - Sizes: the same file list is reported twice, with bytes appended in between. A report that
//    published anything other than a stat — an echo, a cached number, a constant — cannot return
//    two different sizes for two identical inputs, so it fails the second assertion.
//  - Omission: delete the file and the entry must lose `fileSizeBytes` entirely. Publishing 0
//    there (the shape this whole ticket is about) fails `TestFalse` on HasField, not a value
//    comparison, so a zero cannot pass by looking like a small file.
//  - Floor: the same bitrate is scored at two resolutions and must cross the floor in opposite
//    directions. A floor expressed in Mbps rather than per pixel gives the same verdict twice
//    and fails one of them.
//  - Encoder: read off the engine's own CDO, so the assertion is "the shipped default really is
//    Quality/CRF 20" — the trap itself — rather than a value this test supplied.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/MRQ/MRQArtifactReport.h"
// The picture half of the same report: a real PNG is written, decoded by the shipped path and
// measured, so the frame assertions below run against the production analysis rather than a
// stand-in for it.
#include "Handlers/Image/ImageOps.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    FString PWMrqScratchDir()
    {
        return FPaths::ProjectIntermediateDir()
            / TEXT("MRQArtifactReportTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    }

    // Writes a file of exactly ByteCount bytes and returns true on success.
    bool PWMrqWriteFileOfSize(const FString& Path, int32 ByteCount)
    {
        TArray<uint8> Bytes;
        Bytes.Init(static_cast<uint8>(0x5A), ByteCount);
        return FFileHelper::SaveArrayToFile(Bytes, *Path);
    }

    bool PWMrqAppendBytes(const FString& Path, int32 ByteCount)
    {
        TArray<uint8> Bytes;
        Bytes.Init(static_cast<uint8>(0x5A), ByteCount);
        return FFileHelper::SaveArrayToFile(Bytes, *Path, &IFileManager::Get(), FILEWRITE_Append);
    }

    // True when any entry of the report's `warnings` array contains Needle.
    bool PWMrqWarningContains(const TSharedPtr<FJsonObject>& Report, const TCHAR* Needle)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Report.IsValid() || !Report->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && Text.Contains(Needle))
            {
                return true;
            }
        }
        return false;
    }

    // Frame content for the picture-fact tests, all 8-bit grey so the Rec.709 luminance of a pixel
    // IS the value written and every assertion below can be read off the numbers directly.
    //
    // The textured half spreads over 181 distinct levels with ~180 pixels each at 256x256, which
    // is far above both the blank floor and the tone-range floor — so a frame carrying it is
    // healthy by every WHOLE-FRAME statistic the plugin already publishes. That is the point: the
    // void has to be caught by the spatial measure or not at all.
    FColor PWMrqTexturedPixel(int32 X, int32 Y)
    {
        const uint8 Level = static_cast<uint8>(((X * 7 + Y * 13) % 181) + 20);
        return FColor(Level, Level, Level, 255);
    }

    // A frame whose top VoidStartRow rows are textured and whose remainder is one flat colour.
    // VoidStartRow == Height produces the fully textured control frame.
    bool PWMrqWriteFrame(const FString& Path, int32 Size, int32 VoidStartRow, uint8 VoidLevel)
    {
        PinWrightImage::FBitmap Bitmap;
        Bitmap.Width = Size;
        Bitmap.Height = Size;
        Bitmap.Pixels.SetNumUninitialized(Size * Size);
        for (int32 Y = 0; Y < Size; ++Y)
        {
            for (int32 X = 0; X < Size; ++X)
            {
                Bitmap.Pixels[Y * Size + X] = (Y >= VoidStartRow)
                    ? FColor(VoidLevel, VoidLevel, VoidLevel, 255)
                    : PWMrqTexturedPixel(X, Y);
            }
        }
        FString ErrCode;
        FString ErrMsg;
        return PinWrightImage::SaveBitmapPng(Path, Bitmap, ErrCode, ErrMsg);
    }

    bool PWMrqWriteGradientVoidFrame(const FString& Path, int32 Size)
    {
        PinWrightImage::FBitmap Bitmap;
        Bitmap.Width = Size;
        Bitmap.Height = Size;
        Bitmap.Pixels.SetNumUninitialized(Size * Size);
        for (int32 Y = 0; Y < Size; ++Y)
        {
            for (int32 X = 0; X < Size; ++X)
            {
                if (Y < Size / 2)
                {
                    Bitmap.Pixels[Y * Size + X] = PWMrqTexturedPixel(X, Y);
                    continue;
                }
                // Six levels across the lower half plus sparse two-level dither: low local
                // variance, but wide enough in min/max that the old equality detector fragmented
                // the region exactly like the ticket's real pale-grey void.
                const int32 Gradient = ((Y - Size / 2) * 6) / (Size / 2);
                const int32 Dither = ((X * 17 + Y * 31) % 23 == 0) ? 2 : 0;
                const uint8 Level = static_cast<uint8>(230 + Gradient + Dither);
                Bitmap.Pixels[Y * Size + X] = FColor(Level, Level, Level, 255);
            }
        }
        FString ErrCode;
        FString ErrMsg;
        return PinWrightImage::SaveBitmapPng(Path, Bitmap, ErrCode, ErrMsg);
    }

    // The first entry of `outputFiles`, or null.
    const FJsonObject* PWMrqFirstFileEntry(const TSharedPtr<FJsonObject>& Report)
    {
        const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
        if (!Report.IsValid() || !Report->TryGetArrayField(TEXT("outputFiles"), Files)
            || !Files || Files->Num() == 0)
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!(*Files)[0]->TryGetObject(Entry) || !Entry || !Entry->IsValid())
        {
            return nullptr;
        }
        return Entry->Get();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQArtifactSizeAndBitrateAreMeasuredTest,
    "PinWright.mrq.run_jobs.ArtifactSizeAndBitrateAreMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQArtifactSizeAndBitrateAreMeasuredTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = PWMrqScratchDir();
    const FString VideoPath = ScratchRoot / TEXT("Render.mp4");
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    constexpr int32 FirstSizeBytes = 300000;
    if (!TestTrue(TEXT("scratch video file written"), PWMrqWriteFileOfSize(VideoPath, FirstSizeBytes)))
    {
        return true;
    }

    TArray<PinWrightMRQ::FRenderedFile> Files;
    Files.Add(PinWrightMRQ::FRenderedFile{ VideoPath, TEXT("FinalImage") });

    // 120 frames at 60 fps = 2.000 s, 1920x1080 — the shape of the delivered bad file.
    PinWrightMRQ::FEncodeContext Context;
    Context.FrameCount = 120;
    Context.FrameRate = 60.0;
    Context.Width = 1920;
    Context.Height = 1080;

    TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
    if (!TestTrue(TEXT("a report is produced"), Report.IsValid()))
    {
        return true;
    }

    // The path the render wrote, which no field of the old response carried at all.
    const FJsonObject* Entry = PWMrqFirstFileEntry(Report);
    if (TestNotNull(TEXT("outputFiles carries the rendered file"), Entry))
    {
        TestEqual(TEXT("outputFiles[0].path is the file the render wrote"),
            Entry->GetStringField(TEXT("path")), VideoPath);
        TestTrue(TEXT("outputFiles[0].exists is measured true"),
            Entry->GetBoolField(TEXT("exists")));
        TestEqual(TEXT("outputFiles[0].fileSizeBytes matches the bytes written"),
            static_cast<int64>(Entry->GetNumberField(TEXT("fileSizeBytes"))),
            static_cast<int64>(FirstSizeBytes));
        TestEqual(TEXT("outputFiles[0].renderPass names the pass"),
            Entry->GetStringField(TEXT("renderPass")), FString(TEXT("FinalImage")));
    }

    TestEqual(TEXT("totalFileSizeBytes matches the bytes written"),
        static_cast<int64>(Report->GetNumberField(TEXT("totalFileSizeBytes"))),
        static_cast<int64>(FirstSizeBytes));
    TestEqual(TEXT("durationSeconds is frameCount / frameRate"),
        Report->GetNumberField(TEXT("durationSeconds")), 2.0, 0.0001);
    // 300000 bytes * 8 / 2 s = 1,200,000 bps — the same order as the 1.16 Mbps file that shipped.
    TestEqual(TEXT("overallBitrateBps is fileSize*8/duration"),
        Report->GetNumberField(TEXT("overallBitrateBps")), 1200000.0, 1.0);
    // 1200000 / (1920*1080*60) = 0.009645 bpp.
    TestEqual(TEXT("bitsPerPixel is bitrate / (w*h*fps)"),
        Report->GetNumberField(TEXT("bitsPerPixel")), 0.0096451, 0.000001);
    TestTrue(TEXT("an encode below the plausibility floor warns"),
        PWMrqWarningContains(Report, TEXT("plausibility floor")));

    // THE COUNTERFACTUAL. Same file list, same context, more bytes on disk. Only a stat can
    // answer differently the second time.
    constexpr int32 AppendedBytes = 200000;
    if (!TestTrue(TEXT("bytes appended to the scratch file"),
        PWMrqAppendBytes(VideoPath, AppendedBytes)))
    {
        return true;
    }
    TSharedPtr<FJsonObject> Regrown = PinWrightMRQ::BuildArtifactReport(Files, Context);
    if (TestTrue(TEXT("a second report is produced"), Regrown.IsValid()))
    {
        TestEqual(TEXT("totalFileSizeBytes tracks the file, not the request"),
            static_cast<int64>(Regrown->GetNumberField(TEXT("totalFileSizeBytes"))),
            static_cast<int64>(FirstSizeBytes + AppendedBytes));
        TestEqual(TEXT("overallBitrateBps tracks the file, not the request"),
            Regrown->GetNumberField(TEXT("overallBitrateBps")), 2000000.0, 1.0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQArtifactUnmeasurableIsOmittedNotZeroedTest,
    "PinWright.mrq.run_jobs.ArtifactUnmeasurableIsOmittedNotZeroed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQArtifactUnmeasurableIsOmittedNotZeroedTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = PWMrqScratchDir();
    const FString MissingPath = ScratchRoot / TEXT("NeverWritten.mp4");
    const FString PresentPath = ScratchRoot / TEXT("Written.mp4");
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    // A path the pipeline reported but that is not on disk: reported honestly as absent, never
    // as a size of 0. This is the case the ticket calls the silent-false-success class — a size
    // for a file that does not exist would be a measurement of nothing.
    {
        TArray<PinWrightMRQ::FRenderedFile> Files;
        Files.Add(PinWrightMRQ::FRenderedFile{ MissingPath, FString() });

        PinWrightMRQ::FEncodeContext Context;
        Context.FrameCount = 120;
        Context.FrameRate = 60.0;
        Context.Width = 1920;
        Context.Height = 1080;

        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (TestTrue(TEXT("a report is produced for a missing file"), Report.IsValid()))
        {
            const FJsonObject* Entry = PWMrqFirstFileEntry(Report);
            if (TestNotNull(TEXT("the missing path is still listed"), Entry))
            {
                TestFalse(TEXT("exists is measured false"), Entry->GetBoolField(TEXT("exists")));
                TestFalse(TEXT("fileSizeBytes is OMITTED for a file that is not there"),
                    Entry->HasField(TEXT("fileSizeBytes")));
            }
            TestEqual(TEXT("measuredFileCount counts what was stat'd, not what was claimed"),
                static_cast<int32>(Report->GetNumberField(TEXT("measuredFileCount"))), 0);
            TestFalse(TEXT("totalFileSizeBytes is OMITTED when nothing was measured"),
                Report->HasField(TEXT("totalFileSizeBytes")));
            TestFalse(TEXT("overallBitrateBps is OMITTED when nothing was measured"),
                Report->HasField(TEXT("overallBitrateBps")));
            TestTrue(TEXT("the missing file is named in a warning"),
                PWMrqWarningContains(Report, TEXT("not on disk")));
        }
    }

    // A measured file with no duration available: the size is published, the bitrate is not, and
    // the response says why rather than dividing by a zero it invented.
    {
        if (!TestTrue(TEXT("scratch file written"), PWMrqWriteFileOfSize(PresentPath, 4096)))
        {
            return true;
        }
        TArray<PinWrightMRQ::FRenderedFile> Files;
        Files.Add(PinWrightMRQ::FRenderedFile{ PresentPath, FString() });

        // No FrameCount, no FrameRate — the shape when the pipeline never cached work metrics.
        PinWrightMRQ::FEncodeContext Context;
        Context.Width = 1920;
        Context.Height = 1080;

        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (TestTrue(TEXT("a report is produced without a duration"), Report.IsValid()))
        {
            TestEqual(TEXT("the size is still measured"),
                static_cast<int64>(Report->GetNumberField(TEXT("totalFileSizeBytes"))),
                static_cast<int64>(4096));
            TestFalse(TEXT("durationSeconds is OMITTED, not zeroed"),
                Report->HasField(TEXT("durationSeconds")));
            TestFalse(TEXT("overallBitrateBps is OMITTED, not zeroed"),
                Report->HasField(TEXT("overallBitrateBps")));
            TestFalse(TEXT("bitsPerPixel is OMITTED, not zeroed"),
                Report->HasField(TEXT("bitsPerPixel")));
            TestTrue(TEXT("the omission is explained"),
                PWMrqWarningContains(Report, TEXT("OMITTED")));
        }
    }

    // No files reported at all: the report says so instead of returning a clean-looking payload.
    {
        TSharedPtr<FJsonObject> Report =
            PinWrightMRQ::BuildArtifactReport(TArray<PinWrightMRQ::FRenderedFile>(),
                PinWrightMRQ::FEncodeContext());
        if (TestTrue(TEXT("a report is produced for a render that wrote nothing"), Report.IsValid()))
        {
            TestEqual(TEXT("outputFileCount is 0"),
                static_cast<int32>(Report->GetNumberField(TEXT("outputFileCount"))), 0);
            TestFalse(TEXT("totalFileSizeBytes is OMITTED"),
                Report->HasField(TEXT("totalFileSizeBytes")));
            TestTrue(TEXT("the empty result is explained rather than left to look clean"),
                PWMrqWarningContains(Report, TEXT("no output files")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQArtifactPlausibilityFloorIsPerPixelTest,
    "PinWright.mrq.run_jobs.ArtifactPlausibilityFloorIsPerPixel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQArtifactPlausibilityFloorIsPerPixelTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = PWMrqScratchDir();
    const FString VideoPath = ScratchRoot / TEXT("Render.mp4");
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    // 4000 bytes over 2 s = 16,000 bps. The SAME bitrate is implausible at 1920x1080@60
    // (0.00013 bpp) and comfortable at 64x64@30 (0.130 bpp). A floor expressed in Mbps could not
    // tell those apart; only bits per pixel per frame generalises across resolutions.
    if (!TestTrue(TEXT("scratch video file written"), PWMrqWriteFileOfSize(VideoPath, 4000)))
    {
        return true;
    }
    TArray<PinWrightMRQ::FRenderedFile> Files;
    Files.Add(PinWrightMRQ::FRenderedFile{ VideoPath, FString() });

    {
        PinWrightMRQ::FEncodeContext Context;
        Context.FrameCount = 120;
        Context.FrameRate = 60.0;
        Context.Width = 1920;
        Context.Height = 1080;
        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (TestTrue(TEXT("a 1080p60 report is produced"), Report.IsValid()))
        {
            TestTrue(TEXT("16 kbps at 1080p60 is flagged implausible"),
                PWMrqWarningContains(Report, TEXT("plausibility floor")));
        }
    }
    {
        PinWrightMRQ::FEncodeContext Context;
        Context.FrameCount = 60;
        Context.FrameRate = 30.0;
        Context.Width = 64;
        Context.Height = 64;
        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (TestTrue(TEXT("a 64x64@30 report is produced"), Report.IsValid()))
        {
            TestTrue(TEXT("the same bitrate at 64x64@30 measures above the floor"),
                Report->GetNumberField(TEXT("bitsPerPixel")) > PinWrightMRQ::MinPlausibleBitsPerPixel);
            TestFalse(TEXT("and is NOT flagged implausible"),
                PWMrqWarningContains(Report, TEXT("plausibility floor")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQEncoderReadbackReportsShippedDefaultTest,
    "PinWright.mrq.run_jobs.EncoderReadbackReportsShippedDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQEncoderReadbackReportsShippedDefaultTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Quality has no lower bitrate bound"),
        PinWrightMRQ::RateControlIsUnboundedBelow(TEXT("Quality")));
    TestTrue(TEXT("ConstantQP has no lower bitrate bound"),
        PinWrightMRQ::RateControlIsUnboundedBelow(TEXT("ConstantQP")));
    TestFalse(TEXT("VariableBitRate does have one"),
        PinWrightMRQ::RateControlIsUnboundedBelow(TEXT("VariableBitRate")));

    UClass* EncoderClass = FindObject<UClass>(nullptr,
        TEXT("/Script/MovieRenderPipelineMP4Encoder.MoviePipelineMP4EncoderOutput"));
    if (!EncoderClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mp4_encoder_module_absent"),
            TEXT("UMoviePipelineMP4EncoderOutput is not loaded in this build, so the encoder "
                 "read-back was not exercised against the engine class."));
        return true;
    }

    const UObject* EncoderDefaults = EncoderClass->GetDefaultObject();
    if (!TestNotNull(TEXT("the encoder class has a CDO"), EncoderDefaults))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Encoder = PinWrightMRQ::ReadRequestedEncoderSettings(EncoderDefaults);
    if (!TestTrue(TEXT("the encoder settings read back"), Encoder.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("the encoder class is named"),
        Encoder->GetStringField(TEXT("class")), EncoderClass->GetPathName());
    // The trap itself: the engine ships quality-targeted encoding at CRF 20, which places no
    // lower bound on the bitrate a low-detail shot produces. Reading it off the CDO means this
    // assertion tracks the engine rather than a value the test supplied.
    TestEqual(TEXT("the shipped rate control is Quality"),
        Encoder->GetStringField(TEXT("rateControl")), FString(TEXT("Quality")));
    TestEqual(TEXT("the shipped constant rate factor is 20"),
        static_cast<int32>(Encoder->GetNumberField(TEXT("constantRateFactor"))), 20);
    TestTrue(TEXT("the average bitrate knob is read back too"),
        Encoder->HasField(TEXT("averageBitrateMbps")));

    // And the read-back feeds the warning, so a caller on the default is told before shipping.
    const FString ScratchRoot = PWMrqScratchDir();
    const FString VideoPath = ScratchRoot / TEXT("Render.mp4");
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    };
    if (TestTrue(TEXT("scratch video file written"), PWMrqWriteFileOfSize(VideoPath, 4096)))
    {
        TArray<PinWrightMRQ::FRenderedFile> Files;
        Files.Add(PinWrightMRQ::FRenderedFile{ VideoPath, FString() });
        PinWrightMRQ::FEncodeContext Context;
        Context.FrameCount = 60;
        Context.FrameRate = 30.0;
        Context.Width = 64;
        Context.Height = 64;
        Context.RequestedEncoder = Encoder;

        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (TestTrue(TEXT("a report is produced"), Report.IsValid()))
        {
            TestTrue(TEXT("the encoder settings are published as REQUESTED, not as measured"),
                Report->HasField(TEXT("encoderRequested")));
            TestFalse(TEXT("and never under a bare `encoder` name"),
                Report->HasField(TEXT("encoder")));
            // Above the plausibility floor at this resolution, so the only thing that can raise
            // this warning is the rate-control read-back.
            TestTrue(TEXT("a quality-targeted rate control warns on its own"),
                PWMrqWarningContains(Report, TEXT("NO lower bitrate bound")));
        }
    }
    return true;
}

// The mrq.create_job pre-flight, which is ask 2 of the same ticket: everything the artifact report
// above says AFTER the render is spent, said BEFORE it, off the queued job's resolved config.
//
// COUNTERFACTUAL. The rate-control warning is scored in both directions from the SAME builder
// call shape — the engine's own shipped Quality/CRF-20 default must warn, and a VariableBitRate
// read-back must not. A builder that warned unconditionally (or never) fails one of the two.
// Reading the warning direction off the engine CDO rather than a supplied literal is what makes
// the first half pin the trap itself rather than a value this test chose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQPreflightWarnsUnboundedRateControlTest,
    "PinWright.mrq.create_job.PreflightWarnsUnboundedRateControl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQPreflightWarnsUnboundedRateControlTest::RunTest(const FString& Parameters)
{
    // True for a warning list containing Needle.
    auto WarningsContain = [](const TArray<FString>& Warnings, const TCHAR* Needle)
    {
        for (const FString& Warning : Warnings)
        {
            if (Warning.Contains(Needle))
            {
                return true;
            }
        }
        return false;
    };

    // A config that writes nothing is the pre-flight's other honest signal: it renders for minutes
    // and produces no file, while mrq.run_jobs still reports success:true for it.
    {
        PinWrightMRQ::FPreflightContext Context;
        Context.Width = 1920;
        Context.Height = 1080;
        Context.OutputDirectory = TEXT("{project_dir}/Saved/MovieRenders/");
        Context.FileNameFormat = TEXT("{sequence_name}.{frame_number}");
        TArray<FString> Warnings;
        TSharedPtr<FJsonObject> Preflight = PinWrightMRQ::BuildPreflightReport(Context, Warnings);
        if (TestTrue(TEXT("a preflight block is produced"), Preflight.IsValid()))
        {
            // Present and empty, not omitted: "this job writes nothing" is a measurement of the
            // config, and a missing array would read as "not disclosed".
            const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
            if (TestTrue(TEXT("the output list is disclosed even when it is empty"),
                Preflight->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs))
            {
                TestEqual(TEXT("and it is empty"), Outputs->Num(), 0);
            }
            // The path shape is published unresolved on purpose — nothing has expanded these
            // tokens yet, and a resolved-looking filename here would be a claim about a file
            // no code has computed.
            TestEqual(TEXT("the output directory is disclosed as its format string"),
                Preflight->GetStringField(TEXT("outputDirectory")),
                FString(TEXT("{project_dir}/Saved/MovieRenders/")));
            TestFalse(TEXT("no encoder is claimed when no video output is configured"),
                Preflight->HasField(TEXT("encoderRequested")));
        }
        TestTrue(TEXT("a job that will write no files says so before the render"),
            WarningsContain(Warnings, TEXT("no files at all")));
    }

    UClass* EncoderClass = FindObject<UClass>(nullptr,
        TEXT("/Script/MovieRenderPipelineMP4Encoder.MoviePipelineMP4EncoderOutput"));
    if (!EncoderClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mp4_encoder_module_absent"),
            TEXT("UMoviePipelineMP4EncoderOutput is not loaded in this build, so the pre-flight "
                 "rate-control warning was not exercised against the engine's shipped default."));
        return true;
    }
    const UObject* EncoderDefaults = EncoderClass->GetDefaultObject();
    if (!TestNotNull(TEXT("the encoder class has a CDO"), EncoderDefaults))
    {
        return true;
    }

    // Direction 1: the engine's shipped default. The value under test is read off the CDO, so
    // this assertion tracks the engine rather than a literal this test supplied.
    {
        PinWrightMRQ::FPreflightContext Context;
        Context.Width = 1920;
        Context.Height = 1080;
        Context.OutputClassPaths.Add(EncoderClass->GetPathName());
        Context.RequestedEncoder = PinWrightMRQ::ReadRequestedEncoderSettings(EncoderDefaults);
        TArray<FString> Warnings;
        TSharedPtr<FJsonObject> Preflight = PinWrightMRQ::BuildPreflightReport(Context, Warnings);
        if (TestTrue(TEXT("a preflight block is produced for the shipped encoder"),
            Preflight.IsValid()))
        {
            TestTrue(TEXT("the encoder is published as REQUESTED, not as measured"),
                Preflight->HasField(TEXT("encoderRequested")));
            TestFalse(TEXT("and never under a bare `encoder` name"),
                Preflight->HasField(TEXT("encoder")));
        }
        TestTrue(TEXT("the shipped Quality default is flagged before the render is spent"),
            WarningsContain(Warnings, TEXT("NO lower bitrate bound")));
        TestFalse(TEXT("and a configured video output is not reported as writing nothing"),
            WarningsContain(Warnings, TEXT("no files at all")));
    }

    // Direction 2: the remedy the warning names. Same builder, same shape, no warning — so the
    // first half cannot be passing because the builder warns unconditionally.
    {
        TSharedPtr<FJsonObject> Encoder = MakeShared<FJsonObject>();
        Encoder->SetStringField(TEXT("class"), EncoderClass->GetPathName());
        Encoder->SetStringField(TEXT("rateControl"), TEXT("VariableBitRate"));
        Encoder->SetNumberField(TEXT("averageBitrateMbps"), 50.0);

        PinWrightMRQ::FPreflightContext Context;
        Context.Width = 1920;
        Context.Height = 1080;
        Context.OutputClassPaths.Add(EncoderClass->GetPathName());
        Context.RequestedEncoder = Encoder;
        TArray<FString> Warnings;
        PinWrightMRQ::BuildPreflightReport(Context, Warnings);
        TestFalse(TEXT("a bitrate-targeted encode raises no rate-control warning"),
            WarningsContain(Warnings, TEXT("NO lower bitrate bound")));
    }
    return true;
}

// The picture facts, which is the defect one ticket further on: every field the report published
// was a property of the FILE, and four 8.8 MB 4K PNGs at 8.53 bits/pixel came back
// `jobSucceeded: true` with no warnings while the lower 47 % of each was a flat pale-grey void
// (B-mrq-run-jobs-succeeds-on-unrenderable-frames).
//
// COUNTERFACTUALS, and the second one is what makes the first mean anything:
//  - The SAME builder is run over two REAL PNGs, written by this test and decoded by the shipped
//    path, that differ only in whether the lower half is flat. A report that flagged frames
//    unconditionally fails the clean half; one that never looked at pixels fails the void half.
//  - The void frame's WHOLE-FRAME verdicts are asserted to stay clean -- `blank`, `crushed` and
//    `blownOut` all false -- so the test pins that the void is caught by the SPATIAL measure and
//    could not have been caught by the statistics that already existed. If a future change makes
//    `blank` fire here, that assertion fails and the reviewer learns the criterion moved.
//  - `fileSizeBytes` is asserted present and positive on the void frame, i.e. the misleading
//    evidence from the ticket is reproduced in the same response as the correct verdict.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQFrameVoidIsSuspectTest,
    "PinWright.mrq.run_jobs.FrameVoidIsSuspect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQFrameVoidIsSuspectTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = PWMrqScratchDir();
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    constexpr int32 FrameSize = 256;
    constexpr uint8 VoidLevel = 233;
    const FString VoidPath = ScratchRoot / TEXT("Render_0000.png");
    const FString CleanPath = ScratchRoot / TEXT("Render_0001.png");
    if (!TestTrue(TEXT("the half-void frame was written"),
            PWMrqWriteFrame(VoidPath, FrameSize, FrameSize / 2, VoidLevel))
        || !TestTrue(TEXT("the fully textured control frame was written"),
            PWMrqWriteFrame(CleanPath, FrameSize, FrameSize, VoidLevel)))
    {
        return true;
    }

    PinWrightMRQ::FEncodeContext Context;
    Context.Width = FrameSize;
    Context.Height = FrameSize;

    // Direction 1: the frame from the ticket.
    {
        TArray<PinWrightMRQ::FRenderedFile> Files;
        Files.Add(PinWrightMRQ::FRenderedFile{ VoidPath, TEXT("FinalImage") });
        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (!TestTrue(TEXT("a report is produced for the void frame"), Report.IsValid()))
        {
            return true;
        }

        TestEqual(TEXT("the frame was opened, not merely stat'd"),
            static_cast<int32>(Report->GetNumberField(TEXT("framesAnalyzed"))), 1);
        TestEqual(TEXT("and it is counted as suspect"),
            static_cast<int32>(Report->GetNumberField(TEXT("framesSuspect"))), 1);

        const FJsonObject* Entry = PWMrqFirstFileEntry(Report);
        if (!TestNotNull(TEXT("the void frame has an outputFiles entry"), Entry))
        {
            return true;
        }
        TestTrue(TEXT("the pixels were analyzed"), Entry->GetBoolField(TEXT("imageAnalyzed")));
        TestTrue(TEXT("the frame is flagged suspect"), Entry->GetBoolField(TEXT("suspect")));

        // The verdicts that ALREADY existed stay clean, which is why they could not catch this.
        TestFalse(TEXT("`blank` is false and correct - pixels were drawn"),
            Entry->GetBoolField(TEXT("blank")));
        TestFalse(TEXT("`crushed` is false and correct - the frame has full tone range"),
            Entry->GetBoolField(TEXT("crushed")));
        TestFalse(TEXT("`blownOut` is false and correct"),
            Entry->GetBoolField(TEXT("blownOut")));
        // ...and the file facts stay healthy too, exactly as they did in the ticket.
        TestTrue(TEXT("the misleading file evidence is reproduced beside the verdict"),
            Entry->HasField(TEXT("fileSizeBytes"))
                && Entry->GetNumberField(TEXT("fileSizeBytes")) > 0.0);

        const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
        if (TestTrue(TEXT("the reason is published as a machine token"),
            Entry->TryGetArrayField(TEXT("suspectReasons"), Reasons) && Reasons))
        {
            bool bFlatRegion = false;
            for (const TSharedPtr<FJsonValue>& Value : *Reasons)
            {
                FString Reason;
                if (Value.IsValid() && Value->TryGetString(Reason) && Reason == TEXT("FLAT_REGION"))
                {
                    bFlatRegion = true;
                }
            }
            TestTrue(TEXT("and the token is FLAT_REGION, not a whole-frame verdict"), bFlatRegion);
        }

        const TSharedPtr<FJsonObject>* Stats = nullptr;
        if (TestTrue(TEXT("imageStats is published per frame"),
            Entry->TryGetObjectField(TEXT("imageStats"), Stats) && Stats && Stats->IsValid()))
        {
            const double Fraction = (*Stats)->GetNumberField(TEXT("flatRegionFraction"));
            TestTrue(TEXT("the flat region is measured at about half the frame"),
                Fraction > 0.45 && Fraction < 0.55);
            TestEqual(TEXT("at the level the void was painted at"),
                static_cast<int32>((*Stats)->GetNumberField(TEXT("flatRegionLevel"))),
                static_cast<int32>(VoidLevel));

            const TSharedPtr<FJsonObject>* Bounds = nullptr;
            if (TestTrue(TEXT("and the response says WHERE it is"),
                (*Stats)->TryGetObjectField(TEXT("flatRegionBounds"), Bounds)
                    && Bounds && Bounds->IsValid()))
            {
                // The whole remedy for the ticket: a caller reads "everything below half the frame
                // height" off the response instead of opening the file.
                TestTrue(TEXT("the region starts at the vertical midpoint"),
                    FMath::IsNearlyEqual((*Bounds)->GetNumberField(TEXT("minY")), 0.5, 0.02));
                TestTrue(TEXT("and runs to the bottom edge"),
                    FMath::IsNearlyEqual((*Bounds)->GetNumberField(TEXT("maxY")), 1.0, 0.02));
            }
        }

        TestTrue(TEXT("the job warns about the frame, not only about the encode"),
            PWMrqWarningContains(Report, TEXT("SUSPECT")));
        TestTrue(TEXT("and says warm-up cannot be the mechanism when every frame is affected"),
            PWMrqWarningContains(Report, TEXT("EVERY frame that was measured is affected")));
    }

    // Direction 2: the same builder, the same shape, a frame with no void. Without this the block
    // above passes for a builder that flags everything.
    {
        TArray<PinWrightMRQ::FRenderedFile> Files;
        Files.Add(PinWrightMRQ::FRenderedFile{ CleanPath, TEXT("FinalImage") });
        TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
        if (!TestTrue(TEXT("a report is produced for the clean frame"), Report.IsValid()))
        {
            return true;
        }
        TestEqual(TEXT("the clean frame was opened too"),
            static_cast<int32>(Report->GetNumberField(TEXT("framesAnalyzed"))), 1);
        TestEqual(TEXT("and nothing about it is suspect"),
            static_cast<int32>(Report->GetNumberField(TEXT("framesSuspect"))), 0);

        const FJsonObject* Entry = PWMrqFirstFileEntry(Report);
        if (TestNotNull(TEXT("the clean frame has an outputFiles entry"), Entry))
        {
            TestFalse(TEXT("a fully textured frame is not flagged"),
                Entry->GetBoolField(TEXT("suspect")));
            TestFalse(TEXT("and carries no suspectReasons at all"),
                Entry->HasField(TEXT("suspectReasons")));
        }
        TestFalse(TEXT("and the job raises no frame warning"),
            PWMrqWarningContains(Report, TEXT("SUSPECT")));
    }
    return true;
}

// The reviewer-returned fixture: unlike the original constant-colour unit case, the real lower
// half was a shallow gradient with enough dither to fragment a min/max equality detector.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQLowVarianceGradientVoidIsSuspectTest,
    "PinWright.mrq.run_jobs.LowVarianceGradientVoidIsSuspect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQLowVarianceGradientVoidIsSuspectTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = PWMrqScratchDir();
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    };

    constexpr int32 FrameSize = 256;
    const FString FramePath = ScratchRoot / TEXT("GradientVoid_0000.png");
    if (!TestTrue(TEXT("the shallow-gradient void frame was written"),
        PWMrqWriteGradientVoidFrame(FramePath, FrameSize)))
    {
        return true;
    }

    TArray<PinWrightMRQ::FRenderedFile> Files;
    Files.Add(PinWrightMRQ::FRenderedFile{ FramePath, TEXT("FinalImage") });
    PinWrightMRQ::FEncodeContext Context;
    Context.Width = FrameSize;
    Context.Height = FrameSize;
    const TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
    if (!TestTrue(TEXT("the gradient-void report was produced"), Report.IsValid()))
    {
        return true;
    }

    const FJsonObject* Entry = PWMrqFirstFileEntry(Report);
    if (!TestNotNull(TEXT("the gradient void has an output entry"), Entry))
    {
        return true;
    }
    TestTrue(TEXT("low local variance marks the gradient void suspect"),
        Entry->GetBoolField(TEXT("suspect")));
    const TSharedPtr<FJsonObject>* Stats = nullptr;
    if (TestTrue(TEXT("the gradient void carries spatial statistics"),
        Entry->TryGetObjectField(TEXT("imageStats"), Stats) && Stats && Stats->IsValid()))
    {
        const double Fraction = (*Stats)->GetNumberField(TEXT("flatRegionFraction"));
        TestTrue(TEXT("the connected low-variance region covers the lower half"),
            Fraction > 0.45 && Fraction < 0.55);
        const TSharedPtr<FJsonObject>* Bounds = nullptr;
        if (TestTrue(TEXT("the gradient void carries region bounds"),
            (*Stats)->TryGetObjectField(TEXT("flatRegionBounds"), Bounds)
                && Bounds && Bounds->IsValid()))
        {
            TestTrue(TEXT("the region starts near the vertical midpoint"),
                FMath::IsNearlyEqual((*Bounds)->GetNumberField(TEXT("minY")), 0.5, 0.03));
            TestTrue(TEXT("the region reaches the bottom edge"),
                FMath::IsNearlyEqual((*Bounds)->GetNumberField(TEXT("maxY")), 1.0, 0.02));
        }
    }
    return true;
}

// A file the verb cannot decode is reported as NOT LOOKED AT, never as clean. "No verdict" and
// "a good verdict" must not share a representation -- that identity is this ticket, one level
// down.
//
// COUNTERFACTUAL. An implementation that simply skipped what it could not decode would leave the
// video entry indistinguishable from an analyzed-and-fine one: `imageAnalyzed` would be absent
// rather than present-and-false, which is what the second assertion below scores.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQNonImageArtifactIsNotReportedCleanTest,
    "PinWright.mrq.run_jobs.NonImageArtifactIsNotReportedClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQNonImageArtifactIsNotReportedCleanTest::RunTest(const FString& Parameters)
{
    const FString ScratchRoot = PWMrqScratchDir();
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString VideoPath = ScratchRoot / TEXT("Render.mp4");
    if (!TestTrue(TEXT("a video artifact was written"), PWMrqWriteFileOfSize(VideoPath, 4096)))
    {
        return true;
    }

    TArray<PinWrightMRQ::FRenderedFile> Files;
    Files.Add(PinWrightMRQ::FRenderedFile{ VideoPath, TEXT("FinalImage") });
    PinWrightMRQ::FEncodeContext Context;
    Context.Width = 1920;
    Context.Height = 1080;

    TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport(Files, Context);
    if (!TestTrue(TEXT("a report is produced"), Report.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("no frame was opened"),
        static_cast<int32>(Report->GetNumberField(TEXT("framesAnalyzed"))), 0);

    const FJsonObject* Entry = PWMrqFirstFileEntry(Report);
    if (TestNotNull(TEXT("the video has an outputFiles entry"), Entry))
    {
        TestTrue(TEXT("the entry says the pixels were not looked at"),
            Entry->HasField(TEXT("imageAnalyzed")));
        TestFalse(TEXT("and says so as false, not by omission"),
            Entry->GetBoolField(TEXT("imageAnalyzed")));
        TestTrue(TEXT("with the reason named"),
            Entry->HasField(TEXT("imageNotAnalyzedReason")));
        TestFalse(TEXT("and no `suspect` verdict is invented for it"),
            Entry->HasField(TEXT("suspect")));
    }
    return true;
}

// The measurement's own anti-chaining property, asserted directly because a regression here is
// invisible from the outside: it would show up as every gradient sky in every render being
// reported as a void.
//
// A region grows only within FlatRegionToleranceLevels of its SEED block, never of the frontier.
// The fixture is a vertical ramp of one level per block row: EVERY block is individually flat, so
// an implementation that chained against the frontier would merge the whole frame into one region
// (fraction 1.0), while the shipped rule caps a region at the few block rows inside the seed's
// band. The uniform-frame direction underneath rules out a measure that simply never finds
// anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQFlatRegionDoesNotChainAGradientTest,
    "PinWright.mrq.run_jobs.FlatRegionDoesNotChainAGradient",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQFlatRegionDoesNotChainAGradientTest::RunTest(const FString& Parameters)
{
    constexpr int32 Size = 256;
    // 64 block rows of 4 pixels each at this size, so every block spans exactly one ramp step and
    // is therefore flat on its own.
    const int32 BlockRows = PinWrightFlatRegion::MaxBlocksPerAxis;
    const int32 RowsPerStep = Size / BlockRows;

    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Size * Size);
    for (int32 Y = 0; Y < Size; ++Y)
    {
        const uint8 Level = static_cast<uint8>(Y / RowsPerStep);
        for (int32 X = 0; X < Size; ++X)
        {
            Pixels[Y * Size + X] = FColor(Level, Level, Level, 255);
        }
    }

    const PinWrightFlatRegion::FFlatRegionStats Stats =
        PinWrightFlatRegion::MeasureLargestFlatRegion(Pixels, Size, Size);
    if (!TestTrue(TEXT("the gradient was measured"), Stats.bMeasured))
    {
        return true;
    }
    // Every block is flat, and that is the trap: flatness alone says nothing about structure.
    TestEqual(TEXT("every block reads as individually flat"),
        Stats.FlatBlockCount, Stats.BlockCount);
    TestTrue(TEXT("yet no single region spans a meaningful share of the frame"),
        Stats.LargestRegionFraction < 0.15);
    TestFalse(TEXT("so a gradient is never reported as a void"), Stats.bLargeFlatRegion);

    TArray<FColor> Uniform;
    Uniform.Init(FColor(128, 128, 128, 255), Size * Size);
    const PinWrightFlatRegion::FFlatRegionStats UniformStats =
        PinWrightFlatRegion::MeasureLargestFlatRegion(Uniform, Size, Size);
    if (TestTrue(TEXT("the uniform frame was measured"), UniformStats.bMeasured))
    {
        TestTrue(TEXT("a genuinely uniform frame is one region covering all of it"),
            UniformStats.LargestRegionFraction > 0.99);
        TestTrue(TEXT("and is flagged"), UniformStats.bLargeFlatRegion);
    }
    return true;
}
