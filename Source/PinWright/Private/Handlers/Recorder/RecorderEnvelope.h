// Copyright (c) 2026 Alexander Penkin. MIT License.

// Self-describing envelope attached to every recorder query result.
//
// The consumer is a token-bounded LLM: these fields let it understand units, time
// domain, and whether the result was clipped without dumping raw data. One
// BuildEnvelope helper emits the `meta` sub-object every handler attaches.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace RecorderEnvelope
{

// Schema version of the query result envelope; bump on any result-shape change.
constexpr int32 SchemaVersion = 1;

// Field bag for one result's envelope. Unset optional fields are omitted from the
// emitted JSON (rather than serialized null) so the page stays compact.
struct FEnvelopeMeta
{
    bool bHasUnits = false;
    FString Units;

    bool bHasTimeDomain = false;
    FString TimeDomain;

    bool bHasTimeRange = false;
    double TimeRangeFrom = 0.0;
    double TimeRangeTo = 0.0;

    bool bHasReduction = false;
    FString Reduction;

    bool bHasThreshold = false;
    double ThresholdApplied = 0.0;

    int32 Elided = 0;
    bool bTruncated = false;
    int32 TotalCount = 0;

    bool bHasNextCursor = false;
    FString NextCursor;
};

// Render the envelope into a JSON object placed under the result's "meta" field.
TSharedRef<FJsonObject> Build(const FEnvelopeMeta& Meta);

} // namespace RecorderEnvelope
