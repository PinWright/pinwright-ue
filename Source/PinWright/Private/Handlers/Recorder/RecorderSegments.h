// Copyright (c) 2026 Alexander Penkin. MIT License.

// Boundary-event pairing of one session into named, typed segment windows.
//
// A declarative rule table maps host-project gameplay boundary events (round,
// match, lobby, mission, editor-session boundaries, ...) to [tMin, tMax]
// windows whose endpoints feed directly into the
// window-scoped recorder verbs. Unpaired boundaries degrade to half-open
// segments rather than being dropped.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "RecorderSessionModel.h"
#include "RecorderSegmentRule.h"

namespace RecorderSegments
{

using namespace RecorderModel;

// One interior anchor event kept on a segment as a drill-down handle.
struct FSegmentAnchor
{
    int64 EventId = 0;
    double Ts = 0.0;
    FString Name;
    FString Key;
};

// One paired window. Index is 1-based per segment type. bStartOpen/bEndOpen mark
// half-open degradation: the boundary was inferred from the session edge (or a
// restarting start event), not observed as an event.
struct FSegment
{
    FString Type;
    int32 Index = 0;
    double TMin = 0.0;
    double TMax = 0.0;
    bool bStartOpen = false;
    bool bEndOpen = false;
    // Start (or, for start-less degenerates, end) event's props echoed verbatim
    // (run_id, round, editor_session_id, track_uid, ...).
    TSharedPtr<FJsonObject> Props;
    TArray<FSegmentAnchor> AnchorEvents;
};

// Pair the session's events into typed segments using the supplied rule set
// (see RecorderSegmentRegistry::Get for the live host-registered rules).
// Output is sorted by TMin.
TArray<FSegment> PairSegments(const FSessionModel& Session, const TArray<FRecorderSegmentRule>& Rules);

} // namespace RecorderSegments
