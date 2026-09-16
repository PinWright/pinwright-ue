// Copyright (c) 2026 Alexander Penkin. MIT License.

// Summary-first query surface over one parsed FSessionModel.
//
// Every method returns a token-bounded JSON result with a self-describing envelope;
// hard row/time caps are enforced by construction so the LLM consumer can never
// trigger a raw dump. Thin handlers delegate here; this is the unit-tested layer.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "RecorderSessionModel.h"

class FRecorderQueryEngine
{
public:
    // Absolute upper bound on rows any single result may carry, regardless of request.
    static constexpr int32 HardRowCap = 2000;
    // Default series point budget when the caller does not specify one.
    static constexpr int32 DefaultMaxPoints = 200;
    // Default catalog cap for describe_session before objects are elided by activity.
    static constexpr int32 DefaultObjectCatalogCap = 200;
    // Default and hard caps for find_events.
    static constexpr int32 DefaultEventLimit = 100;
    // Significance epsilon used when a tag's manifest declared none.
    static constexpr double DefaultEpsilon = 0.01;

    explicit FRecorderQueryEngine(const TSharedRef<const RecorderModel::FSessionModel>& InSession)
        : Session(InSession)
    {
    }

    // Session manifest: object catalog (capped, top-N by activity), per-tag variable
    // manifest, time domain, and event count. When bHasFrom/bHasTo gate a window,
    // the activity ranking and event count are scoped to it (e.g. one segment from
    // ListSegments); otherwise they cover the whole session.
    TSharedRef<FJsonObject> Describe(int32 ObjectCatalogCap = DefaultObjectCatalogCap,
                                     bool bHasFrom = false, double From = 0.0,
                                     bool bHasTo = false, double To = 0.0) const;

    // As-of values of one object's tags at a timestamp. Empty Tags means every tag
    // recorded on the object.
    TSharedRef<FJsonObject> GetState(const FString& Key, const TArray<FString>& Tags, double AtTs) const;

    // Endpoint + extremum delta summary of one (key, tag) over [From, To].
    TSharedRef<FJsonObject> SummarizeChange(const FString& Key, const FString& Tag, double From, double To) const;

    // Reduced series of one (key, tag) over [From, To]. Reduction is "downsample",
    // "diff", or "summary" (treated as downsample). Hard-capped + cursor-paginated.
    TSharedRef<FJsonObject> GetSeries(const FString& Key, const FString& Tag, double From, double To,
                                      const FString& Reduction, int32 MaxPoints, const FString& Cursor) const;

    // Events matching a filter, ordered by timestamp, anchor-frame aware.
    // bHasFrom/bHasTo gate the time window; bHasSeverityMin gates the severity floor.
    // Cursor is an offset into the sorted match list; the meta carries nextCursor
    // while matches remain.
    TSharedRef<FJsonObject> FindEvents(const FString& Name, bool bHasSeverityMin, int32 SeverityMin,
                                       const FString& Key, bool bHasFrom, double From, bool bHasTo, double To,
                                       int32 Limit, const FString& Cursor) const;

    // Boundary events paired into typed segment windows, using the host-registered
    // rule set (see RecorderSegmentRegistry). Each segment's tMin/tMax feed directly
    // into the window-scoped verbs.
    TSharedRef<FJsonObject> ListSegments() const;

private:
    TSharedRef<const RecorderModel::FSessionModel> Session;
};

namespace RecorderQuery
{
    // Map a severity name ("Trace".."Fatal") to its ordinal level. Unknown -> INDEX_NONE.
    int32 SeverityLevelFromName(const FString& Name);
}
