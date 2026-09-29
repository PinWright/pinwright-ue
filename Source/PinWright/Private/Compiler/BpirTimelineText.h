// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirTimelineText.h - BPIR text form of a Timeline template: the settings and tracks carried as
// `timeline Name(...)` args. The decompiler formats them and the compiler parses them, so the two
// directions share one grammar and a decompile/recompile keeps every track.

#pragma once

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"
#include "Engine/TimelineTemplate.h"

struct FBpirArg;
class UCurveBase;

namespace BpirTimelineText
{
    struct FTrackSpec
    {
        FName Name;
        FTTTrackBase::ETrackType Type = FTTTrackBase::TT_FloatInterp;
        // Set for a track that references a curve asset instead of owning its keys.
        UCurveBase* ExternalCurve = nullptr;
        // Float and event tracks use channel 0, vector tracks x/y/z, color tracks r/g/b/a.
        TArray<FRichCurveKey> Channels[4];
    };

    struct FSpec
    {
        TOptional<float> Length;
        TOptional<ETimelineLengthMode> LengthMode;
        TOptional<bool> bAutoPlay;
        TOptional<bool> bLoop;
        TOptional<bool> bReplicated;
        TOptional<bool> bIgnoreTimeDilation;
        TArray<FTrackSpec> Tracks;
    };

    // The args between the parens of `timeline Name(...)`: settings that differ from a new
    // template, then every track in display order. Empty for a new, empty template.
    FString FormatArgs(UTimelineTemplate* Template);

    // Parses timeline args. Returns false with OutError naming the first malformed arg.
    bool ParseArgs(const TArray<FBpirArg>& Args, FSpec& OutSpec, FString& OutError);

    // Writes a parsed spec onto a template fresh from FBlueprintEditorUtils::AddNewTimeline.
    // Owned curves are created under CurveOuter (the Blueprint's generated class, as the
    // Timeline editor does).
    void Apply(const FSpec& Spec, UTimelineTemplate* Template, UObject* CurveOuter);
}
