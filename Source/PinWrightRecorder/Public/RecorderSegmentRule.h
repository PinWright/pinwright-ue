// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Host-supplied description of how journal boundary events pair into segment
// windows. These are pure data — the editor-side query layer (in the gateway
// module) does the actual event matching and pairing against a recorded session.
//
// A host project registers its gameplay-specific rules (race rounds, matches,
// missions, ...) via RecorderSegmentRegistry::Register from its own module
// startup, keeping that vocabulary in the host where the boundary events are
// emitted. The recorder module ships only the engine-generic editor_session rule
// as a built-in default, so a project that registers nothing still gets sensible
// editor-session pairing and never inherits another project's vocabulary.

// Matches a journal event by exact name, plus an optional string-prop
// discriminator for single-name boundary pairs (e.g. editor:session action=open/exit).
struct FRecorderEventMatcher
{
    FString Name;
    FString PropName;
    FString PropValue;
};

// One pairing rule: how a segment type opens, what attaches as an interior
// anchor, how it closes, and how concurrent instances correlate.
struct FRecorderSegmentRule
{
    FString Type;
    TArray<FRecorderEventMatcher> Starts;
    TArray<FRecorderEventMatcher> Anchors;
    TArray<FRecorderEventMatcher> Ends;
    // Prop whose value pairs an end with its start (e.g. editor_session_id);
    // empty means at most one instance of the type is open at a time.
    FString CorrelationProp;
    // True when a repeated start while open attaches as an anchor instead of
    // restarting (e.g. every joining player emits the same lobby-join event).
    bool bIgnoreStartWhileOpen = false;
};

namespace RecorderSegmentRegistry
{
    // Append a host rule. Call once per rule at module startup. Not thread-safe;
    // intended for the game thread during module init, before any query runs.
    PINWRIGHTRECORDER_API void Register(const FRecorderSegmentRule& Rule);

    // The full rule set: the built-in editor_session default plus every rule a
    // host has registered, in registration order (default first).
    PINWRIGHTRECORDER_API const TArray<FRecorderSegmentRule>& Get();

    // Drop all host registrations and restore the default-only set. For tests.
    PINWRIGHTRECORDER_API void ResetToDefault();
}
