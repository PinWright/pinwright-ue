// Copyright (c) 2026 Alexander Penkin. MIT License.

// ISO base-media (MP4 / MOV) track-list reader for the mrq.run_jobs artifact report. See
// MRQContainerProbe.h for why it exists and what it promises. Box layouts follow ISO/IEC 14496-12
// (sample entries, mdhd, hdlr, stsz) and 14496-1 (esds descriptors).
#include "Handlers/MRQ/MRQContainerProbe.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Templates/UniquePtr.h"

namespace PinWrightMRQContainerProbeLocal
{
    // `moov` is metadata only — sample tables, not samples — so a cap far above any render's
    // header keeps a corrupt size field from allocating the whole file.
    constexpr int64 MaxMoovBytes = 64 * 1024 * 1024;

    // Bounds-checked big-endian reads over one in-memory buffer, limited to the end of the box
    // being read (Within). Every read reports failure rather than reading past that end, so a
    // short or truncated box makes the field absent instead of reading its next sibling's bytes.
    struct FBoxBytes
    {
        const TArray<uint8>& Data;
        int64 Limit = 0;

        FBoxBytes Within(int64 End) const
        {
            return FBoxBytes{ Data, FMath::Min(End, Limit) };
        }
        bool Has(int64 Offset, int64 Count) const
        {
            return Offset >= 0 && Count >= 0 && Offset + Count <= Limit;
        }
        bool U8(int64 Offset, uint32& Out) const
        {
            if (!Has(Offset, 1)) { return false; }
            Out = Data[Offset];
            return true;
        }
        bool U16(int64 Offset, uint32& Out) const
        {
            if (!Has(Offset, 2)) { return false; }
            Out = (uint32(Data[Offset]) << 8) | uint32(Data[Offset + 1]);
            return true;
        }
        bool U32(int64 Offset, uint32& Out) const
        {
            if (!Has(Offset, 4)) { return false; }
            Out = (uint32(Data[Offset]) << 24) | (uint32(Data[Offset + 1]) << 16)
                | (uint32(Data[Offset + 2]) << 8) | uint32(Data[Offset + 3]);
            return true;
        }
        bool U64(int64 Offset, uint64& Out) const
        {
            uint32 High = 0;
            uint32 Low = 0;
            if (!U32(Offset, High) || !U32(Offset + 4, Low)) { return false; }
            Out = (uint64(High) << 32) | uint64(Low);
            return true;
        }
        FString FourCC(int64 Offset) const
        {
            if (!Has(Offset, 4)) { return FString(); }
            FString Out;
            for (int32 Index = 0; Index < 4; ++Index)
            {
                Out.AppendChar(static_cast<TCHAR>(Data[Offset + Index]));
            }
            return Out;
        }
    };

    struct FBoxSpan
    {
        uint32 Type = 0;
        int64 PayloadBegin = 0;
        int64 End = 0;
    };

    constexpr uint32 BoxType(const char* Tag)
    {
        return (uint32(uint8(Tag[0])) << 24) | (uint32(uint8(Tag[1])) << 16)
            | (uint32(uint8(Tag[2])) << 8) | uint32(uint8(Tag[3]));
    }

    // The child boxes of [Begin, End). False when a box header is truncated or a box claims more
    // bytes than its parent holds.
    bool ListBoxes(const FBoxBytes& Bytes, int64 Begin, int64 End, TArray<FBoxSpan>& OutBoxes)
    {
        int64 Pos = Begin;
        while (Pos < End)
        {
            uint32 Size32 = 0;
            if (End - Pos < 8 || !Bytes.U32(Pos, Size32))
            {
                return false;
            }
            int64 Header = 8;
            int64 Size = Size32;
            if (Size32 == 1)
            {
                uint64 Size64 = 0;
                if (!Bytes.U64(Pos + 8, Size64)) { return false; }
                Header = 16;
                Size = static_cast<int64>(Size64);
            }
            else if (Size32 == 0)
            {
                Size = End - Pos;
            }
            if (Size < Header || Size > End - Pos)
            {
                return false;
            }
            uint32 Type = 0;
            Bytes.U32(Pos + 4, Type);
            OutBoxes.Add(FBoxSpan{ Type, Pos + Header, Pos + Size });
            Pos += Size;
        }
        return true;
    }

    const FBoxSpan* FindBox(const TArray<FBoxSpan>& Boxes, uint32 Type)
    {
        return Boxes.FindByPredicate([Type](const FBoxSpan& Box) { return Box.Type == Type; });
    }

    // One MPEG-4 descriptor length: up to four bytes, 7 bits each, high bit = "more follows".
    bool ReadDescriptorLength(const FBoxBytes& Bytes, int64& Pos, uint32& OutLength)
    {
        OutLength = 0;
        for (int32 Index = 0; Index < 4; ++Index)
        {
            uint32 Byte = 0;
            if (!Bytes.U8(Pos++, Byte)) { return false; }
            OutLength = (OutLength << 7) | (Byte & 0x7F);
            if ((Byte & 0x80) == 0) { return true; }
        }
        return true;
    }

    // `esds` -> ES_Descriptor (tag 3) -> DecoderConfigDescriptor (tag 4) -> objectTypeIndication.
    bool ReadEsdsObjectType(const FBoxBytes& Parent, const FBoxSpan& Esds, uint32& OutObjectType)
    {
        const FBoxBytes Bytes = Parent.Within(Esds.End);
        int64 Pos = Esds.PayloadBegin + 4; // version + flags
        uint32 Tag = 0;
        uint32 Length = 0;
        if (!Bytes.U8(Pos++, Tag) || Tag != 0x03 || !ReadDescriptorLength(Bytes, Pos, Length))
        {
            return false;
        }
        uint32 Flags = 0;
        Pos += 2; // ES_ID
        if (!Bytes.U8(Pos++, Flags)) { return false; }
        if (Flags & 0x80) { Pos += 2; } // dependsOn_ES_ID
        if (Flags & 0x40)
        {
            uint32 UrlLength = 0;
            if (!Bytes.U8(Pos++, UrlLength)) { return false; }
            Pos += UrlLength;
        }
        if (Flags & 0x20) { Pos += 2; } // OCR_ES_Id
        if (!Bytes.U8(Pos++, Tag) || Tag != 0x04 || !ReadDescriptorLength(Bytes, Pos, Length))
        {
            return false;
        }
        return Bytes.U8(Pos, OutObjectType);
    }

    // ISO/IEC 14496-1 objectTypeIndication values that name one codec.
    FString CodecNameForObjectType(uint32 ObjectType)
    {
        switch (ObjectType)
        {
        case 0x20: return TEXT("mpeg4");
        case 0x21: return TEXT("h264");
        case 0x23: return TEXT("hevc");
        case 0x40: case 0x66: case 0x67: case 0x68: return TEXT("aac");
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: return TEXT("mpeg2video");
        case 0x69: case 0x6B: return TEXT("mp3");
        case 0x6A: return TEXT("mpeg1video");
        case 0xA5: return TEXT("ac3");
        case 0xA6: return TEXT("eac3");
        default: return FString();
        }
    }

    // Sample-entry tags that name exactly one codec on their own.
    FString CodecNameForTag(const FString& Tag)
    {
        static const TMap<FString, FString> Names = {
            { TEXT("avc1"), TEXT("h264") }, { TEXT("avc3"), TEXT("h264") },
            { TEXT("hvc1"), TEXT("hevc") }, { TEXT("hev1"), TEXT("hevc") },
            { TEXT("av01"), TEXT("av1") }, { TEXT("vp09"), TEXT("vp9") },
            { TEXT("apch"), TEXT("prores") }, { TEXT("apcn"), TEXT("prores") },
            { TEXT("apcs"), TEXT("prores") }, { TEXT("apco"), TEXT("prores") },
            { TEXT("ap4h"), TEXT("prores") }, { TEXT("ap4x"), TEXT("prores") },
            { TEXT("ac-3"), TEXT("ac3") }, { TEXT("ec-3"), TEXT("eac3") },
            { TEXT("Opus"), TEXT("opus") }, { TEXT("fLaC"), TEXT("flac") },
        };
        const FString* Found = Names.Find(Tag);
        return Found ? *Found : FString();
    }

    // The first sample entry of `stsd`: its tag, dimensions or channel layout, and the codec name
    // where it is unambiguous.
    void ReadSampleEntry(const FBoxBytes& Parent, const FBoxSpan& Stsd,
        PinWrightMRQ::FContainerStream& Stream)
    {
        const FBoxBytes StsdBytes = Parent.Within(Stsd.End);
        uint32 EntryCount = 0;
        uint32 EntrySize = 0;
        const int64 Entry = Stsd.PayloadBegin + 8; // version/flags + entry_count
        if (!StsdBytes.U32(Stsd.PayloadBegin + 4, EntryCount) || EntryCount == 0
            || !StsdBytes.U32(Entry, EntrySize) || EntrySize < 16 || Entry + EntrySize > Stsd.End)
        {
            return;
        }
        const int64 EntryEnd = Entry + EntrySize;
        // Every fixed-offset field below is read inside the sample entry and nowhere else.
        const FBoxBytes Bytes = Parent.Within(EntryEnd);
        Stream.CodecTag = Bytes.FourCC(Entry + 4);
        Stream.CodecName = CodecNameForTag(Stream.CodecTag);

        // Where the entry's own child boxes (esds, ...) start, past its fixed fields.
        int64 ChildBegin = INDEX_NONE;
        uint32 A = 0;
        uint32 B = 0;
        if (Stream.HandlerType == TEXT("vide"))
        {
            // VisualSampleEntry: 8-byte SampleEntry base, 16 reserved, width, height, ...
            if (Bytes.U16(Entry + 32, A) && Bytes.U16(Entry + 34, B))
            {
                Stream.Width = static_cast<int32>(A);
                Stream.Height = static_cast<int32>(B);
            }
            ChildBegin = Entry + 86;
        }
        else if (Stream.HandlerType == TEXT("soun"))
        {
            // AudioSampleEntry. The leading u16 is the QuickTime sound-description version: v1
            // adds 16 bytes of fields and v2 replaces the layout, so only v0/v1 publish the rate.
            uint32 Version = 0;
            if (Bytes.U16(Entry + 16, Version) && Version <= 1)
            {
                if (Bytes.U16(Entry + 24, A)) { Stream.Channels = static_cast<int32>(A); }
                if (Bytes.U32(Entry + 32, B)) { Stream.SampleRate = static_cast<double>(B >> 16); }
            }
            ChildBegin = Entry + 36 + (Version == 1 ? 16 : Version == 2 ? 36 : 0);
        }

        if ((Stream.CodecTag == TEXT("mp4a") || Stream.CodecTag == TEXT("mp4v"))
            && ChildBegin != INDEX_NONE && ChildBegin <= EntryEnd)
        {
            TArray<FBoxSpan> Children;
            if (ListBoxes(Bytes, ChildBegin, EntryEnd, Children))
            {
                uint32 ObjectType = 0;
                if (const FBoxSpan* Esds = FindBox(Children, BoxType("esds")))
                {
                    if (ReadEsdsObjectType(Bytes, *Esds, ObjectType))
                    {
                        Stream.CodecName = CodecNameForObjectType(ObjectType);
                    }
                }
            }
        }
    }

    void ReadSampleSizes(const FBoxBytes& Parent, const FBoxSpan& Stsz,
        PinWrightMRQ::FContainerStream& Stream)
    {
        const FBoxBytes Bytes = Parent.Within(Stsz.End);
        uint32 SampleSize = 0;
        uint32 SampleCount = 0;
        if (!Bytes.U32(Stsz.PayloadBegin + 4, SampleSize)
            || !Bytes.U32(Stsz.PayloadBegin + 8, SampleCount))
        {
            return;
        }
        // A fragmented file keeps its samples in `moof`, leaving zero here; zero bytes over a
        // real duration would publish a 0 bps stream, so no size is published instead.
        if (SampleCount == 0)
        {
            Stream.SampleCount = 0;
            return;
        }
        int64 Total = static_cast<int64>(SampleSize) * SampleCount;
        if (SampleSize == 0)
        {
            const int64 Table = Stsz.PayloadBegin + 12;
            for (uint32 Index = 0; Index < SampleCount; ++Index)
            {
                uint32 Size = 0;
                if (!Bytes.U32(Table + int64(Index) * 4, Size))
                {
                    return; // a short table publishes neither the count nor the size
                }
                Total += Size;
            }
        }
        Stream.SampleCount = SampleCount;
        Stream.StreamSizeBytes = Total;
    }

    bool ReadTrack(const FBoxBytes& Bytes, const FBoxSpan& Trak,
        PinWrightMRQ::FContainerStream& Stream)
    {
        TArray<FBoxSpan> TrakChildren;
        TArray<FBoxSpan> MdiaChildren;
        const FBoxSpan* Mdia = nullptr;
        if (!ListBoxes(Bytes, Trak.PayloadBegin, Trak.End, TrakChildren)
            || (Mdia = FindBox(TrakChildren, BoxType("mdia"))) == nullptr
            || !ListBoxes(Bytes, Mdia->PayloadBegin, Mdia->End, MdiaChildren))
        {
            return false;
        }
        const FBoxSpan* Hdlr = FindBox(MdiaChildren, BoxType("hdlr"));
        if (!Hdlr)
        {
            return false;
        }
        Stream.HandlerType = Bytes.Within(Hdlr->End).FourCC(Hdlr->PayloadBegin + 8);
        if (Stream.HandlerType.IsEmpty())
        {
            return false;
        }

        if (const FBoxSpan* Mdhd = FindBox(MdiaChildren, BoxType("mdhd")))
        {
            const FBoxBytes MdhdBytes = Bytes.Within(Mdhd->End);
            uint32 Version = 0;
            uint32 Timescale = 0;
            uint64 Duration = 0;
            bool bRead = false;
            if (MdhdBytes.U8(Mdhd->PayloadBegin, Version) && Version == 1)
            {
                bRead = MdhdBytes.U32(Mdhd->PayloadBegin + 20, Timescale)
                    && MdhdBytes.U64(Mdhd->PayloadBegin + 24, Duration)
                    && Duration != MAX_uint64;
            }
            else
            {
                uint32 Duration32 = 0;
                bRead = MdhdBytes.U32(Mdhd->PayloadBegin + 12, Timescale)
                    && MdhdBytes.U32(Mdhd->PayloadBegin + 16, Duration32)
                    && Duration32 != MAX_uint32;
                Duration = Duration32;
            }
            if (bRead && Timescale > 0 && Duration > 0)
            {
                Stream.DurationSeconds = static_cast<double>(Duration) / Timescale;
            }
        }

        TArray<FBoxSpan> MinfChildren;
        TArray<FBoxSpan> StblChildren;
        const FBoxSpan* Minf = FindBox(MdiaChildren, BoxType("minf"));
        const FBoxSpan* Stbl = nullptr;
        if (Minf && ListBoxes(Bytes, Minf->PayloadBegin, Minf->End, MinfChildren)
            && (Stbl = FindBox(MinfChildren, BoxType("stbl"))) != nullptr
            && ListBoxes(Bytes, Stbl->PayloadBegin, Stbl->End, StblChildren))
        {
            if (const FBoxSpan* Stsd = FindBox(StblChildren, BoxType("stsd")))
            {
                ReadSampleEntry(Bytes, *Stsd, Stream);
            }
            if (const FBoxSpan* Stsz = FindBox(StblChildren, BoxType("stsz")))
            {
                ReadSampleSizes(Bytes, *Stsz, Stream);
            }
        }
        return true;
    }
}

FString PinWrightMRQ::FContainerStream::CodecType() const
{
    if (HandlerType == TEXT("vide")) { return TEXT("video"); }
    if (HandlerType == TEXT("soun")) { return TEXT("audio"); }
    if (HandlerType == TEXT("text") || HandlerType == TEXT("sbtl") || HandlerType == TEXT("subt"))
    {
        return TEXT("subtitle");
    }
    return TEXT("data");
}

TOptional<double> PinWrightMRQ::FContainerStream::BitrateBps() const
{
    if (StreamSizeBytes.IsSet() && DurationSeconds.IsSet() && DurationSeconds.GetValue() > 0.0)
    {
        return static_cast<double>(StreamSizeBytes.GetValue()) * 8.0 / DurationSeconds.GetValue();
    }
    return TOptional<double>();
}

bool PinWrightMRQ::PathIsIsoMediaContainer(const FString& Path)
{
    const FString Extension = FPaths::GetExtension(Path).ToLower();
    return Extension == TEXT("mp4") || Extension == TEXT("m4v") || Extension == TEXT("m4a")
        || Extension == TEXT("mov");
}

bool PinWrightMRQ::ReadContainerStreams(const FString& Path, TArray<FContainerStream>& OutStreams,
    FString& OutReason)
{
    using namespace PinWrightMRQContainerProbeLocal;
    OutStreams.Reset();

    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
    if (!Reader)
    {
        OutReason = TEXT("The file could not be opened for reading.");
        return false;
    }

    // Top level: read each box header and seek past its payload, so only `moov` is ever loaded.
    const int64 FileSize = Reader->TotalSize();
    int64 Pos = 0;
    TArray<uint8> Moov;
    bool bFoundMoov = false;
    while (Pos + 8 <= FileSize)
    {
        TArray<uint8> Header;
        Header.SetNumZeroed(16);
        const int64 HeaderBytes = FMath::Min<int64>(16, FileSize - Pos);
        Reader->Seek(Pos);
        Reader->Serialize(Header.GetData(), HeaderBytes);
        if (Reader->IsError())
        {
            OutReason = FString::Printf(TEXT("Reading the box header at byte %lld failed."), Pos);
            return false;
        }
        const FBoxBytes HeaderView{ Header, Header.Num() };
        uint32 Size32 = 0;
        HeaderView.U32(0, Size32);
        const FString Type = HeaderView.FourCC(4);
        for (const TCHAR Char : Type)
        {
            if (Char < 0x20 || Char > 0x7E)
            {
                OutReason = FString::Printf(
                    TEXT("Byte %lld does not start an ISO base-media box (type is not printable), so ")
                    TEXT("this is not an MP4/MOV box stream."), Pos);
                return false;
            }
        }
        int64 BoxHeader = 8;
        int64 BoxSize = Size32;
        if (Size32 == 1)
        {
            uint64 Size64 = 0;
            if (HeaderBytes < 16 || !HeaderView.U64(8, Size64))
            {
                OutReason = TEXT("A 64-bit box size is truncated.");
                return false;
            }
            BoxHeader = 16;
            BoxSize = static_cast<int64>(Size64);
        }
        else if (Size32 == 0)
        {
            BoxSize = FileSize - Pos;
        }
        if (BoxSize < BoxHeader || BoxSize > FileSize - Pos)
        {
            OutReason = FString::Printf(
                TEXT("The '%s' box at byte %lld claims %lld bytes, past the end of the %lld-byte ")
                TEXT("file (truncated or not a box stream)."), *Type, Pos, BoxSize, FileSize);
            return false;
        }
        if (Type == TEXT("moov"))
        {
            const int64 PayloadSize = BoxSize - BoxHeader;
            if (PayloadSize > MaxMoovBytes)
            {
                OutReason = FString::Printf(
                    TEXT("The 'moov' box is %lld bytes, above the %lld-byte read cap."),
                    PayloadSize, MaxMoovBytes);
                return false;
            }
            Moov.SetNumUninitialized(PayloadSize);
            Reader->Seek(Pos + BoxHeader);
            Reader->Serialize(Moov.GetData(), PayloadSize);
            if (Reader->IsError())
            {
                OutReason = TEXT("Reading the 'moov' box failed.");
                return false;
            }
            bFoundMoov = true;
            break;
        }
        Pos += BoxSize;
    }
    if (!bFoundMoov)
    {
        OutReason = TEXT("No 'moov' box was found, so the file carries no track list to read.");
        return false;
    }

    const FBoxBytes Bytes{ Moov, Moov.Num() };
    TArray<FBoxSpan> MoovChildren;
    if (!ListBoxes(Bytes, 0, Moov.Num(), MoovChildren))
    {
        OutReason = TEXT("A box inside 'moov' overruns its parent.");
        return false;
    }
    for (const FBoxSpan& Box : MoovChildren)
    {
        if (Box.Type != BoxType("trak"))
        {
            continue;
        }
        FContainerStream Stream;
        if (!ReadTrack(Bytes, Box, Stream))
        {
            OutReason = FString::Printf(
                TEXT("Track %d has no readable 'mdia'/'hdlr' box."), OutStreams.Num());
            OutStreams.Reset();
            return false;
        }
        OutStreams.Add(MoveTemp(Stream));
    }
    return true;
}

TSharedPtr<FJsonObject> PinWrightMRQ::ContainerStreamToJson(const FContainerStream& Stream,
    int32 Index)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetNumberField(TEXT("index"), Index);
    Out->SetStringField(TEXT("codecType"), Stream.CodecType());
    Out->SetStringField(TEXT("handlerType"), Stream.HandlerType);
    if (!Stream.CodecTag.IsEmpty())
    {
        Out->SetStringField(TEXT("codecTag"), Stream.CodecTag);
    }
    if (!Stream.CodecName.IsEmpty())
    {
        Out->SetStringField(TEXT("codecName"), Stream.CodecName);
    }
    if (Stream.Width.IsSet() && Stream.Height.IsSet())
    {
        Out->SetNumberField(TEXT("width"), Stream.Width.GetValue());
        Out->SetNumberField(TEXT("height"), Stream.Height.GetValue());
    }
    if (Stream.Channels.IsSet())
    {
        Out->SetNumberField(TEXT("channels"), Stream.Channels.GetValue());
    }
    if (Stream.SampleRate.IsSet())
    {
        Out->SetNumberField(TEXT("sampleRate"), Stream.SampleRate.GetValue());
    }
    if (Stream.DurationSeconds.IsSet())
    {
        Out->SetNumberField(TEXT("durationSeconds"), Stream.DurationSeconds.GetValue());
    }
    if (Stream.SampleCount.IsSet())
    {
        Out->SetNumberField(TEXT("sampleCount"), static_cast<double>(Stream.SampleCount.GetValue()));
    }
    if (Stream.StreamSizeBytes.IsSet())
    {
        Out->SetNumberField(TEXT("streamSizeBytes"),
            static_cast<double>(Stream.StreamSizeBytes.GetValue()));
    }
    if (const TOptional<double> Bitrate = Stream.BitrateBps(); Bitrate.IsSet())
    {
        Out->SetNumberField(TEXT("bitrateBps"), Bitrate.GetValue());
    }
    return Out;
}
