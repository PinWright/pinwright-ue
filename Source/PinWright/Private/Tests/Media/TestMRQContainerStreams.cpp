// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for what the mrq.run_jobs artifact report says a movie file CONTAINS, and for the config
// disclosure (`sampling`, `settings`) mirrored into both reports.
//
// THE DEFECT THESE PIN. The report stat'd each file and never opened one, so a 1080p60 MP4 that
// carried a 192 kbit/s AAC track of pure silence beside the video read exactly like a video-only
// render, and its `overallBitrateBps` silently included the audio
// (B-mrq-artifact-report-omits-stream-count). And the anti-aliasing sample and warm-up counts that
// decide convergence were read by no verb (B-mrq-config-readback-omits-sampling).
//
// WHY HAND-BUILT CONTAINERS. UE's MRQ MP4 writer is a stub on Linux (GenericPlatform
// MoviePipelineMP4Encoder: "MP4 Encoder is unsupported on this platform"), and this host has no
// ffmpeg, so no real MRQ MP4 can be produced here; the fixtures below are byte-exact ISO base-media files assembled box by box (ISO/IEC 14496-12), so
// every number the report publishes is one this file put there and can assert exactly. They go
// through the production BuildArtifactReport, which stats and opens them like any render output.
//
// COUNTERFACTUALS:
//  - Streams: the A/V file must publish two streams with their codecs, and the video-only file
//    one. A report that never opened the file publishes neither; one that counted boxes without
//    reading `hdlr` cannot tell them apart.
//  - Audio warning: the same A/V file is reported twice, once with the sequence declared silent
//    and once with it declared to have audio. A warning raised on the stream alone, or never,
//    fails one of the two; the video-only file must raise none.
//  - Bitrate: `videoBitrateBps` must equal the video track's own bytes over its own duration and
//    sit BELOW the whole-file figure, and bitsPerPixel must be computed from it.
//  - Unreadable: a .mp4 of junk bytes must OMIT `streams` and the stream counts, with a reason —
//    publishing an empty array or a 0 count would read as "no audio" about a file never read.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/MRQ/MRQArtifactReport.h"
#include "Tests/TestSkipReporting.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace TestMRQContainerStreamsHelpers
{
    void PutU8(TArray<uint8>& Out, uint32 Value) { Out.Add(static_cast<uint8>(Value)); }
    void PutU16(TArray<uint8>& Out, uint32 Value) { PutU8(Out, Value >> 8); PutU8(Out, Value); }
    void PutU32(TArray<uint8>& Out, uint32 Value) { PutU16(Out, Value >> 16); PutU16(Out, Value); }
    void PutZeros(TArray<uint8>& Out, int32 Count) { Out.AddZeroed(Count); }
    void PutTag(TArray<uint8>& Out, const char* Tag)
    {
        for (int32 Index = 0; Index < 4; ++Index) { PutU8(Out, static_cast<uint8>(Tag[Index])); }
    }

    TArray<uint8> Box(const char* Type, const TArray<uint8>& Payload)
    {
        TArray<uint8> Out;
        PutU32(Out, 8 + Payload.Num());
        PutTag(Out, Type);
        Out.Append(Payload);
        return Out;
    }

    struct FTrackSpec
    {
        const char* Handler = "vide";
        const char* CodecTag = "avc1";
        uint32 Timescale = 30;
        uint32 Duration = 60;
        // Per-sample size table (stsz sample_size 0) when non-empty, else ConstantSampleSize.
        TArray<uint32> SampleSizes;
        uint32 ConstantSampleSize = 0;
        uint32 ConstantSampleCount = 0;
        uint32 Width = 64;
        uint32 Height = 48;
        uint32 Channels = 2;
        uint32 SampleRate = 48000;
        // mp4a/mp4v only: the esds objectTypeIndication.
        uint32 ObjectType = 0x40;
        // An 8-byte mdhd followed by minf: the fixed-offset timescale/duration reads land in
        // minf's header and stbl's size unless they are bounded by mdhd's own end.
        bool bShortMdhd = false;
    };

    TArray<uint8> SampleEntry(const FTrackSpec& Spec)
    {
        TArray<uint8> Body;
        PutZeros(Body, 6);       // reserved
        PutU16(Body, 1);         // data_reference_index
        if (FCStringAnsi::Strcmp(Spec.Handler, "vide") == 0)
        {
            PutZeros(Body, 16);  // pre_defined / reserved
            PutU16(Body, Spec.Width);
            PutU16(Body, Spec.Height);
            PutU32(Body, 0x00480000);
            PutU32(Body, 0x00480000);
            PutU32(Body, 0);
            PutU16(Body, 1);     // frame_count
            PutZeros(Body, 32);  // compressorname
            PutU16(Body, 0x18);  // depth
            PutU16(Body, 0xFFFF);
        }
        else
        {
            PutU16(Body, 0);     // QuickTime sound version 0
            PutZeros(Body, 6);   // revision + vendor
            PutU16(Body, Spec.Channels);
            PutU16(Body, 16);    // samplesize
            PutU32(Body, 0);     // compression id + packet size
            PutU32(Body, Spec.SampleRate << 16);
        }
        if (FCStringAnsi::Strcmp(Spec.CodecTag, "mp4a") == 0)
        {
            // esds: ES_Descriptor(3) { ES_ID, flags, DecoderConfigDescriptor(4) { objectType, ... } }
            TArray<uint8> Esds;
            PutU32(Esds, 0);     // version + flags
            PutU8(Esds, 0x03); PutU8(Esds, 0x15);
            PutU16(Esds, 1); PutU8(Esds, 0);
            PutU8(Esds, 0x04); PutU8(Esds, 0x0D);
            PutU8(Esds, Spec.ObjectType); PutU8(Esds, 0x15);
            PutZeros(Esds, 3); PutU32(Esds, 192000); PutU32(Esds, 192000);
            PutU8(Esds, 0x06); PutU8(Esds, 0x01); PutU8(Esds, 0x02);
            Body.Append(Box("esds", Esds));
        }
        return Box(Spec.CodecTag, Body);
    }

    TArray<uint8> Track(const FTrackSpec& Spec)
    {
        TArray<uint8> Mdhd;
        PutU32(Mdhd, 0); PutU32(Mdhd, 0); PutU32(Mdhd, 0);
        PutU32(Mdhd, Spec.Timescale); PutU32(Mdhd, Spec.Duration);
        PutU16(Mdhd, 0x55C4); PutU16(Mdhd, 0);

        TArray<uint8> Hdlr;
        PutU32(Hdlr, 0); PutU32(Hdlr, 0); PutTag(Hdlr, Spec.Handler);
        PutZeros(Hdlr, 12); PutU8(Hdlr, 0);

        TArray<uint8> Stsd;
        PutU32(Stsd, 0); PutU32(Stsd, 1);
        Stsd.Append(SampleEntry(Spec));

        TArray<uint8> Stsz;
        PutU32(Stsz, 0);
        if (Spec.SampleSizes.Num() > 0)
        {
            PutU32(Stsz, 0); PutU32(Stsz, Spec.SampleSizes.Num());
            for (const uint32 Size : Spec.SampleSizes) { PutU32(Stsz, Size); }
        }
        else
        {
            PutU32(Stsz, Spec.ConstantSampleSize); PutU32(Stsz, Spec.ConstantSampleCount);
        }

        TArray<uint8> Stbl = Box("stsd", Stsd);
        Stbl.Append(Box("stsz", Stsz));
        if (Spec.bShortMdhd)
        {
            Mdhd.SetNum(8);
        }
        TArray<uint8> Mdia = Box("mdhd", Mdhd);
        if (Spec.bShortMdhd)
        {
            Mdia.Append(Box("minf", Box("stbl", Stbl)));
            Mdia.Append(Box("hdlr", Hdlr));
        }
        else
        {
            Mdia.Append(Box("hdlr", Hdlr));
            Mdia.Append(Box("minf", Box("stbl", Stbl)));
        }
        return Box("trak", Box("mdia", Mdia));
    }

    // A video track of 60 samples x 1000 B over 2 s (240,000 bps).
    FTrackSpec VideoTrack()
    {
        FTrackSpec Spec;
        Spec.SampleSizes.Init(1000, 60);
        return Spec;
    }

    // A stereo 48 kHz AAC track of 94 x 512 B over 2 s (192,512 bps).
    FTrackSpec AudioTrack()
    {
        FTrackSpec Spec;
        Spec.Handler = "soun";
        Spec.CodecTag = "mp4a";
        Spec.Timescale = 48000;
        Spec.Duration = 96000;
        Spec.ConstantSampleSize = 512;
        Spec.ConstantSampleCount = 94;
        return Spec;
    }

    // ftyp + (mdat, moov) in either order; mdat holds as many bytes as the sample tables claim.
    TArray<uint8> Container(const TArray<FTrackSpec>& Tracks, bool bMoovFirst)
    {
        TArray<uint8> Ftyp;
        PutTag(Ftyp, "mp42"); PutU32(Ftyp, 0); PutTag(Ftyp, "mp42"); PutTag(Ftyp, "isom");

        TArray<uint8> MoovPayload;
        int32 MediaBytes = 0;
        for (const FTrackSpec& Spec : Tracks)
        {
            MoovPayload.Append(Track(Spec));
            for (const uint32 Size : Spec.SampleSizes) { MediaBytes += Size; }
            MediaBytes += Spec.ConstantSampleSize * Spec.ConstantSampleCount;
        }
        TArray<uint8> MdatPayload;
        MdatPayload.AddZeroed(MediaBytes);

        TArray<uint8> File = Box("ftyp", Ftyp);
        const TArray<uint8> Moov = Box("moov", MoovPayload);
        const TArray<uint8> Mdat = Box("mdat", MdatPayload);
        File.Append(bMoovFirst ? Moov : Mdat);
        File.Append(bMoovFirst ? Mdat : Moov);
        return File;
    }

    FString ScratchDir()
    {
        return FPaths::ProjectIntermediateDir()
            / TEXT("MRQContainerStreamTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    }

    bool WarningContains(const TSharedPtr<FJsonObject>& Report, const TCHAR* Needle)
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

    TSharedPtr<FJsonObject> FirstFile(const TSharedPtr<FJsonObject>& Report)
    {
        const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
        if (!Report.IsValid() || !Report->TryGetArrayField(TEXT("outputFiles"), Files) || !Files
            || Files->Num() == 0)
        {
            return nullptr;
        }
        return (*Files)[0]->AsObject();
    }

    TSharedPtr<FJsonObject> ReportFor(const FString& Path, TOptional<bool> SequenceHasAudio)
    {
        TArray<PinWrightMRQ::FRenderedFile> Files;
        Files.Add(PinWrightMRQ::FRenderedFile{ Path, TEXT("FinalImage") });
        PinWrightMRQ::FEncodeContext Context;
        Context.FrameCount = 60;
        Context.FrameRate = 30.0;
        Context.Width = 64;
        Context.Height = 48;
        Context.SequenceHasAudio = SequenceHasAudio;
        return PinWrightMRQ::BuildArtifactReport(Files, Context);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQContainerStreamsArePublishedTest,
    "PinWright.mrq.run_jobs.ContainerStreamsArePublished",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQContainerStreamsArePublishedTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQContainerStreamsHelpers;
    const FString Root = ScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    const FString Path = Root / TEXT("Render.mp4");
    // moov AFTER mdat: where a streaming writer puts it, so the top-level walk has to seek.
    if (!TestTrue(TEXT("A/V container fixture written"),
        FFileHelper::SaveArrayToFile(Container({ VideoTrack(), AudioTrack() }, false), *Path)))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Report = ReportFor(Path, false);
    const TSharedPtr<FJsonObject> Entry = FirstFile(Report);
    const TArray<TSharedPtr<FJsonValue>>* Streams = nullptr;
    if (!TestTrue(TEXT("the output file entry carries `streams`"),
            Entry.IsValid() && Entry->TryGetArrayField(TEXT("streams"), Streams) && Streams)
        || !TestEqual(TEXT("both tracks are listed"), Streams->Num(), 2))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Video = (*Streams)[0]->AsObject();
    TestEqual(TEXT("stream 0 is video"), Video->GetStringField(TEXT("codecType")), FString(TEXT("video")));
    TestEqual(TEXT("stream 0 tag read off stsd"), Video->GetStringField(TEXT("codecTag")), FString(TEXT("avc1")));
    TestEqual(TEXT("stream 0 codec named"), Video->GetStringField(TEXT("codecName")), FString(TEXT("h264")));
    TestEqual(TEXT("video width read off the sample entry"), static_cast<int32>(Video->GetNumberField(TEXT("width"))), 64);
    TestEqual(TEXT("video height read off the sample entry"), static_cast<int32>(Video->GetNumberField(TEXT("height"))), 48);
    TestEqual(TEXT("video duration demuxed from mdhd"), Video->GetNumberField(TEXT("durationSeconds")), 2.0);
    TestEqual(TEXT("video bytes summed from the stsz table"), Video->GetNumberField(TEXT("streamSizeBytes")), 60000.0);
    TestEqual(TEXT("video bitrate is its own bytes over its own duration"),
        Video->GetNumberField(TEXT("bitrateBps")), 240000.0);

    const TSharedPtr<FJsonObject> Audio = (*Streams)[1]->AsObject();
    TestEqual(TEXT("stream 1 is audio"), Audio->GetStringField(TEXT("codecType")), FString(TEXT("audio")));
    TestEqual(TEXT("mp4a is named from its esds object type"), Audio->GetStringField(TEXT("codecName")), FString(TEXT("aac")));
    TestEqual(TEXT("audio channels read"), static_cast<int32>(Audio->GetNumberField(TEXT("channels"))), 2);
    TestEqual(TEXT("audio sample rate read"), Audio->GetNumberField(TEXT("sampleRate")), 48000.0);
    TestEqual(TEXT("constant-size stsz multiplied out"), Audio->GetNumberField(TEXT("streamSizeBytes")), 48128.0);

    TestEqual(TEXT("job counts one video stream"), static_cast<int32>(Report->GetNumberField(TEXT("videoStreamCount"))), 1);
    TestEqual(TEXT("job counts one audio stream"), static_cast<int32>(Report->GetNumberField(TEXT("audioStreamCount"))), 1);
    TestEqual(TEXT("videoBitrateBps is the video track's"), Report->GetNumberField(TEXT("videoBitrateBps")), 240000.0);
    TestTrue(TEXT("the whole-file bitrate still counts the audio, so it is larger"),
        Report->GetNumberField(TEXT("overallBitrateBps")) > Report->GetNumberField(TEXT("videoBitrateBps")));
    TestTrue(TEXT("bitsPerPixel is computed from the video stream, not the whole file"),
        FMath::IsNearlyEqual(Report->GetNumberField(TEXT("bitsPerPixel")), 240000.0 / (64.0 * 48.0 * 30.0), 1e-9));

    // Same file, both answers about the sequence: only "no audio in the sequence" (or unknown)
    // makes the track a surprise.
    TestTrue(TEXT("an audio stream on a sequence with no audio warns"),
        WarningContains(Report, TEXT("carries no audio track")));
    TestFalse(TEXT("the same stream on a sequence WITH audio does not"),
        WarningContains(ReportFor(Path, true), TEXT("carry an audio stream")));
    TestTrue(TEXT("an undetermined sequence still warns, and says it was not determined"),
        WarningContains(ReportFor(Path, TOptional<bool>()), TEXT("was not determined")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQVideoOnlyContainerRaisesNoAudioWarningTest,
    "PinWright.mrq.run_jobs.VideoOnlyContainerRaisesNoAudioWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQVideoOnlyContainerRaisesNoAudioWarningTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQContainerStreamsHelpers;
    const FString Root = ScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    const FString Path = Root / TEXT("Render.mov");
    if (!TestTrue(TEXT("video-only container fixture written (moov first)"),
        FFileHelper::SaveArrayToFile(Container({ VideoTrack() }, true), *Path)))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Report = ReportFor(Path, false);
    const TSharedPtr<FJsonObject> Entry = FirstFile(Report);
    const TArray<TSharedPtr<FJsonValue>>* Streams = nullptr;
    if (TestTrue(TEXT("the entry carries `streams`"),
        Entry.IsValid() && Entry->TryGetArrayField(TEXT("streams"), Streams) && Streams))
    {
        TestEqual(TEXT("exactly one stream"), Streams->Num(), 1);
    }
    TestEqual(TEXT("audioStreamCount is a measured 0, published"),
        static_cast<int32>(Report->GetNumberField(TEXT("audioStreamCount"))), 0);
    TestFalse(TEXT("no audio warning for a file with no audio stream"),
        WarningContains(Report, TEXT("carry an audio stream")));
    TestTrue(TEXT("video-only: video and whole-file bitrate are both published"),
        Report->HasField(TEXT("videoBitrateBps")) && Report->HasField(TEXT("overallBitrateBps")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQUnreadableContainerOmitsStreamsTest,
    "PinWright.mrq.run_jobs.UnreadableContainerOmitsStreams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQUnreadableContainerOmitsStreamsTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQContainerStreamsHelpers;
    const FString Root = ScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

    // Junk bytes under a movie extension, and a real container cut off inside its moov.
    const FString JunkPath = Root / TEXT("Junk.mp4");
    TArray<uint8> Junk;
    Junk.Init(0x5A, 4096);
    TArray<uint8> Truncated = Container({ VideoTrack(), AudioTrack() }, false);
    Truncated.SetNum(Truncated.Num() - 40);
    const FString TruncatedPath = Root / TEXT("Truncated.mp4");
    if (!TestTrue(TEXT("fixtures written"), FFileHelper::SaveArrayToFile(Junk, *JunkPath)
        && FFileHelper::SaveArrayToFile(Truncated, *TruncatedPath)))
    {
        return true;
    }

    for (const FString& Path : { JunkPath, TruncatedPath })
    {
        const TSharedPtr<FJsonObject> Report = ReportFor(Path, false);
        const TSharedPtr<FJsonObject> Entry = FirstFile(Report);
        if (!TestTrue(FString::Printf(TEXT("%s has an entry"), *Path), Entry.IsValid()))
        {
            continue;
        }
        TestFalse(FString::Printf(TEXT("%s: `streams` is omitted, not published empty"), *Path),
            Entry->HasField(TEXT("streams")));
        TestTrue(FString::Printf(TEXT("%s: the reason is named"), *Path),
            Entry->HasField(TEXT("streamsNotReadReason")));
        TestFalse(FString::Printf(TEXT("%s: no audioStreamCount for an unread file"), *Path),
            Report->HasField(TEXT("audioStreamCount")));
        TestFalse(FString::Printf(TEXT("%s: no videoBitrateBps for an unread file"), *Path),
            Report->HasField(TEXT("videoBitrateBps")));
        TestTrue(FString::Printf(TEXT("%s: the omission is a warning"), *Path),
            WarningContains(Report, TEXT("could not be read")));
    }
    return true;
}

// Box sizes are honoured in both directions: a legal 64-bit size parses, an impossible one and a
// child that overruns its parent are refused, and a box too short for its fixed fields yields an
// ABSENT field rather than bytes read out of the next sibling.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQContainerBoxSizesAreHonouredTest,
    "PinWright.mrq.run_jobs.ContainerBoxSizesAreHonoured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQContainerBoxSizesAreHonouredTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQContainerStreamsHelpers;
    const FString Root = ScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

    TArray<uint8> Ftyp;
    PutTag(Ftyp, "mp42"); PutU32(Ftyp, 0); PutTag(Ftyp, "isom");
    const TArray<uint8> FtypBox = Box("ftyp", Ftyp);
    auto LargeSizeHeader = [](TArray<uint8>& Out, const char* Type, uint64 Size)
    {
        PutU32(Out, 1);
        PutTag(Out, Type);
        PutU32(Out, static_cast<uint32>(Size >> 32));
        PutU32(Out, static_cast<uint32>(Size));
    };

    // 1. mdat with a 64-bit size, then moov: parses.
    TArray<uint8> LargeOk = FtypBox;
    LargeSizeHeader(LargeOk, "mdat", 16 + 60000);
    LargeOk.AddZeroed(60000);
    LargeOk.Append(Box("moov", Track(VideoTrack())));
    // 2. A 64-bit size of 2^63: refused, not wrapped negative and walked.
    TArray<uint8> LargeBad = FtypBox;
    LargeSizeHeader(LargeBad, "mdat", 0x8000000000000000ull);
    LargeBad.AddZeroed(32);
    // 3. A trak whose size claims more than its moov holds.
    TArray<uint8> Overrun = FtypBox;
    TArray<uint8> TrakBytes = Track(VideoTrack());
    const uint32 TrakSize = static_cast<uint32>(TrakBytes.Num()) + 100;
    TrakBytes[0] = TrakSize >> 24; TrakBytes[1] = TrakSize >> 16; TrakBytes[2] = TrakSize >> 8; TrakBytes[3] = TrakSize;
    Overrun.Append(Box("moov", TrakBytes));
    // 4. An mdhd too short for its timescale/duration.
    FTrackSpec ShortMdhd = VideoTrack();
    ShortMdhd.bShortMdhd = true;
    TArray<uint8> Short = FtypBox;
    Short.Append(Box("moov", Track(ShortMdhd)));

    const FString LargeOkPath = Root / TEXT("LargeOk.mp4");
    const FString LargeBadPath = Root / TEXT("LargeBad.mp4");
    const FString OverrunPath = Root / TEXT("Overrun.mp4");
    const FString ShortPath = Root / TEXT("ShortMdhd.mp4");
    if (!TestTrue(TEXT("fixtures written"),
        FFileHelper::SaveArrayToFile(LargeOk, *LargeOkPath) && FFileHelper::SaveArrayToFile(LargeBad, *LargeBadPath)
        && FFileHelper::SaveArrayToFile(Overrun, *OverrunPath) && FFileHelper::SaveArrayToFile(Short, *ShortPath)))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> OkEntry = FirstFile(ReportFor(LargeOkPath, false));
    const TArray<TSharedPtr<FJsonValue>>* Streams = nullptr;
    TestTrue(TEXT("a legal 64-bit mdat size is walked past to the moov"),
        OkEntry.IsValid() && OkEntry->TryGetArrayField(TEXT("streams"), Streams) && Streams && Streams->Num() == 1);

    for (const FString& Path : { LargeBadPath, OverrunPath })
    {
        const TSharedPtr<FJsonObject> Entry = FirstFile(ReportFor(Path, false));
        TestTrue(FString::Printf(TEXT("%s: refused with a reason, no streams"), *FPaths::GetCleanFilename(Path)),
            Entry.IsValid() && !Entry->HasField(TEXT("streams")) && Entry->HasField(TEXT("streamsNotReadReason")));
    }

    const TSharedPtr<FJsonObject> ShortEntry = FirstFile(ReportFor(ShortPath, false));
    Streams = nullptr;
    if (TestTrue(TEXT("the short-mdhd track is still listed"),
        ShortEntry.IsValid() && ShortEntry->TryGetArrayField(TEXT("streams"), Streams) && Streams && Streams->Num() == 1))
    {
        const TSharedPtr<FJsonObject> Stream = (*Streams)[0]->AsObject();
        TestEqual(TEXT("its sample entry is still read"), Stream->GetStringField(TEXT("codecTag")), FString(TEXT("avc1")));
        TestFalse(TEXT("its duration is ABSENT, not read out of the next sibling box"),
            Stream->HasField(TEXT("durationSeconds")));
    }
    return true;
}

// The config half: `sampling` and `settings` are mirrored into BOTH reports from the same reader,
// and `sampling` is absent — not defaulted — when the config carries no anti-aliasing setting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQSamplingIsReadOffTheSettingTest,
    "PinWright.mrq.run_jobs.SamplingAndSettingsAreMirroredIntoBothReports",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQSamplingIsReadOffTheSettingTest::RunTest(const FString& Parameters)
{
    UClass* AntiAliasingClass = FindObject<UClass>(nullptr,
        TEXT("/Script/MovieRenderPipelineCore.MoviePipelineAntiAliasingSetting"));
    if (!AntiAliasingClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_core_module_absent"),
            TEXT("UMoviePipelineAntiAliasingSetting is not loaded in this build, so the sampling "
                 "read-back was not exercised."));
        return true;
    }

    // Planted values no default carries, written through reflection the same way a preset holds
    // them, so the read-back cannot pass by echoing a CDO.
    UObject* Setting = NewObject<UObject>(GetTransientPackage(), AntiAliasingClass);
    TStrongObjectPtr<UObject> SettingGuard(Setting);
    const TPair<const TCHAR*, int32> Planted[] = {
        { TEXT("SpatialSampleCount"), 3 }, { TEXT("TemporalSampleCount"), 7 },
        { TEXT("EngineWarmUpCount"), 41 }, { TEXT("RenderWarmUpCount"), 5 } };
    for (const TPair<const TCHAR*, int32>& Pair : Planted)
    {
        FIntProperty* Property = CastField<FIntProperty>(AntiAliasingClass->FindPropertyByName(Pair.Key));
        if (!TestNotNull(FString::Printf(TEXT("%s exists as an int property"), Pair.Key), Property))
        {
            return true;
        }
        Property->SetPropertyValue_InContainer(Setting, Pair.Value);
    }

    const TSharedPtr<FJsonObject> Sampling = PinWrightMRQ::ReadSamplingSettings(Setting);
    if (!TestTrue(TEXT("sampling read back"), Sampling.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("class named"), Sampling->GetStringField(TEXT("class")), AntiAliasingClass->GetPathName());
    TestEqual(TEXT("spatialSampleCount read"), static_cast<int32>(Sampling->GetNumberField(TEXT("spatialSampleCount"))), 3);
    TestEqual(TEXT("temporalSampleCount read"), static_cast<int32>(Sampling->GetNumberField(TEXT("temporalSampleCount"))), 7);
    TestEqual(TEXT("engineWarmUpCount read"), static_cast<int32>(Sampling->GetNumberField(TEXT("engineWarmUpCount"))), 41);
    TestEqual(TEXT("renderWarmUpCount read"), static_cast<int32>(Sampling->GetNumberField(TEXT("renderWarmUpCount"))), 5);
    TestTrue(TEXT("booleans are read as booleans"), Sampling->HasTypedField<EJson::Boolean>(TEXT("useCameraCutForWarmUp")));
    TestTrue(TEXT("the AA method is read as its enumerator name"),
        Sampling->HasTypedField<EJson::String>(TEXT("antiAliasingMethod")));
    // Every key the authoring verbs accept is one the readback publishes, and vice versa.
    for (const PinWrightMRQ::FSettingField& Field : PinWrightMRQ::SamplingSettingFields())
    {
        TestTrue(FString::Printf(TEXT("readback publishes `%s`"), Field.Key), Sampling->HasField(Field.Key));
    }

    // Preflight: present with a setting, ABSENT without one; `settings` always an array.
    PinWrightMRQ::FPreflightContext With;
    With.Sampling = Sampling;
    With.SettingClassPaths = { AntiAliasingClass->GetPathName() };
    TArray<FString> Warnings;
    const TSharedPtr<FJsonObject> PreflightWith = PinWrightMRQ::BuildPreflightReport(With, Warnings);
    TestTrue(TEXT("preflight publishes sampling when the config has the setting"), PreflightWith->HasField(TEXT("sampling")));
    const TArray<TSharedPtr<FJsonValue>>* Settings = nullptr;
    TestTrue(TEXT("preflight lists every setting class"),
        PreflightWith->TryGetArrayField(TEXT("settings"), Settings) && Settings && Settings->Num() == 1);
    const TSharedPtr<FJsonObject> PreflightWithout =
        PinWrightMRQ::BuildPreflightReport(PinWrightMRQ::FPreflightContext(), Warnings);
    TestFalse(TEXT("preflight OMITS sampling when the config has no AA setting"), PreflightWithout->HasField(TEXT("sampling")));

    // Run-time report: the same block, mirrored.
    PinWrightMRQ::FEncodeContext Context;
    Context.Sampling = Sampling;
    Context.SettingClassPaths = TArray<FString>{ AntiAliasingClass->GetPathName() };
    const TSharedPtr<FJsonObject> Report = PinWrightMRQ::BuildArtifactReport({}, Context);
    const TSharedPtr<FJsonObject>* Mirrored = nullptr;
    if (TestTrue(TEXT("run_jobs report mirrors sampling"),
        Report->TryGetObjectField(TEXT("sampling"), Mirrored) && Mirrored && Mirrored->IsValid()))
    {
        TestEqual(TEXT("mirrored temporal count"),
            static_cast<int32>((*Mirrored)->GetNumberField(TEXT("temporalSampleCount"))), 7);
    }
    TestTrue(TEXT("run_jobs report mirrors settings"), Report->HasField(TEXT("settings")));
    TestFalse(TEXT("an unread config publishes no `settings` claim at run time"),
        PinWrightMRQ::BuildArtifactReport({}, PinWrightMRQ::FEncodeContext())->HasField(TEXT("settings")));
    return true;
}
