// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// What a rendered movie file CONTAINS, read off its own track boxes, for the mrq.run_jobs
// artifact report.
//
// WHAT WENT WRONG. The artifact report stat'd every file and never opened one, so the number and
// kind of streams in the deliverable was not a fact any mrq verb published. A 1080p60 MP4 came
// back with a 192 kbit/s stereo AAC track of pure digital silence riding beside the video — the
// MP4 writer's `bIncludeAudio` defaults to true — and the response read exactly like a clean
// video-only render (B-mrq-artifact-report-omits-stream-count). It also made `overallBitrateBps`
// a whole-file figure that silently includes that audio.
//
// NO DEMUXER, NO NEW MODULE. MP4 / MOV / M4V are ISO base-media files: a sequence of
// size-prefixed boxes, with the track list in `moov` (`trak` > `mdia` > `hdlr` / `mdhd` /
// `minf` > `stbl` > `stsd` / `stsz`). Reading it is a box walk: the top level is skipped box by
// box with a seek (so a `moov` written AFTER `mdat`, which is where a streaming writer puts it,
// costs the same as one at the front), and only `moov` itself is read into memory.
//
// MEASURED OR OMITTED. Every field is taken from the file. A field the file does not carry — a
// duration the track left at zero, a sample table in a box form this reader does not walk
// (`stz2`), a codec tag whose name would be a guess — is left unset rather than filled in.
namespace PinWrightMRQ
{
    struct FContainerStream
    {
        // `hdlr` handler type, verbatim: "vide", "soun", "text", "tmcd", ...
        FString HandlerType;
        // First `stsd` sample-entry four-character code, verbatim: "avc1", "mp4a", "apcn", ...
        FString CodecTag;
        // Common codec name, only where the tag (plus, for mp4a/mp4v, the `esds` object type)
        // names exactly one codec. Empty otherwise.
        FString CodecName;
        TOptional<int32> Width;
        TOptional<int32> Height;
        TOptional<int32> Channels;
        TOptional<double> SampleRate;
        // `mdhd` duration / timescale — DEMUXED, unlike the report's derived durationSeconds.
        TOptional<double> DurationSeconds;
        TOptional<int64> SampleCount;
        // Sum of the track's `stsz` sample sizes: the bytes this one stream occupies.
        TOptional<int64> StreamSizeBytes;

        // "video" / "audio" / "subtitle" / "data", from HandlerType.
        FString CodecType() const;
        // StreamSizeBytes * 8 / DurationSeconds, when both were measured and are positive.
        TOptional<double> BitrateBps() const;
    };

    // True for the extensions this reader understands (.mp4, .m4v, .m4a, .mov). Content is still
    // verified: a file with the extension that is not a box stream fails ReadContainerStreams.
    bool PathIsIsoMediaContainer(const FString& Path);

    // Reads the track list. Returns false with OutReason set when the file could not be opened or
    // is not a readable ISO base-media file (no `moov`, a box that overruns its parent, ...);
    // OutStreams is then empty and must be reported as UNKNOWN, never as "no streams".
    bool ReadContainerStreams(const FString& Path, TArray<FContainerStream>& OutStreams,
        FString& OutReason);

    // The `streams[]` entry published per output file.
    TSharedPtr<FJsonObject> ContainerStreamToJson(const FContainerStream& Stream, int32 Index);
}
