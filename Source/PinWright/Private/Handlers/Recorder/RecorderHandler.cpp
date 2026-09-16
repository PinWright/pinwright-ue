// Copyright (c) 2026 Alexander Penkin. MIT License.

// Structured query RPCs over recorded journal NDJSON sessions (file-only v1).
//
// recorder.list_sessions     - enumerate session files on disk
// recorder.describe_session  - mandatory-first manifest (object catalog + variables)
// recorder.get_state         - as-of values of one object's tags at a timestamp
// recorder.summarize_change  - delta summary of one (key, tag) over a window
// recorder.get_series        - reduced, capped series of one (key, tag)
// recorder.find_events       - locate anchor moments by event filter
// recorder.list_segments     - pair boundary events into typed segment windows
//
// Handlers stay thin; the algorithms live in FRecorderQueryEngine so they are
// unit-testable without the editor / file IO.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "RecorderResolver.h"
#include "RecorderSessionLoader.h"
#include "RecorderQueryEngine.h"

namespace
{

// Resolve + load the `session` argument into an engine. On failure sends the error
// response and returns false (the handler should then return true to the dispatcher).
bool ResolveEngine(const FHandlerContext& Ctx, const FString& Session, TUniquePtr<FRecorderQueryEngine>& OutEngine)
{
    if (Session.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("'session' is required: a recording file path or id."));
        return false;
    }

    const FString Path = RecorderResolver::ResolvePath(Session);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("SESSION_NOT_FOUND"),
            FString::Printf(TEXT("Recording session not found: '%s'."), *Session));
        return false;
    }

    FString LoadError;
    TSharedPtr<RecorderModel::FSessionModel> Model = RecorderSessionLoader::Load(Path, LoadError);
    if (!Model.IsValid())
    {
        Ctx.SendError(TEXT("SESSION_LOAD_FAILED"), LoadError);
        return false;
    }

    OutEngine = MakeUnique<FRecorderQueryEngine>(Model.ToSharedRef());
    return true;
}

} // namespace

REGISTER_RPC_HANDLER("recorder.list_sessions", "recorder",
    "List discoverable journal-recorder sessions under Saved/PinWright/Recordings, newest first; by default only "
    "the 20 most recent are returned (totalCount always reports the full untruncated count). "
    "Each entry gives the id and absolute path — use one as the 'session' argument of the other recorder tools.",
    RPC_PARAMS(
        RPC_PARAM_DEF("limit", "number", "Max sessions to return, newest first. 0 = all.", "20")
    ))
{
    const TArray<RecorderResolver::FSessionListing> Sessions = RecorderResolver::ListSessions();

    const int32 Limit = Ctx.GetInt(TEXT("limit"), 20);
    const int32 Shown = (Limit <= 0) ? Sessions.Num() : FMath::Min(Limit, Sessions.Num());

    TArray<TSharedPtr<FJsonValue>> SessionsJson;
    SessionsJson.Reserve(Shown);
    for (int32 Index = 0; Index < Shown; ++Index)
    {
        const RecorderResolver::FSessionListing& Listing = Sessions[Index];
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("id"), Listing.Id);
        Obj->SetStringField(TEXT("path"), Listing.Path);
        Obj->SetNumberField(TEXT("modifiedUtc"), Listing.ModifiedUtcSeconds);
        SessionsJson.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("sessions"), SessionsJson);
    Result->SetNumberField(TEXT("totalCount"), Sessions.Num());
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("recorder.describe_session", "recorder",
    "MANDATORY FIRST CALL for any recorder query. Returns the session manifest: the object catalog "
    "(key/label/path/type/first-timestamp, ranked by activity and capped), the per-tag variable manifest "
    "(tag/kind/unit/epsilon), the time domain, and the total event count. Use the returned keys and tags to "
    "drive recorder.get_state, recorder.summarize_change, recorder.get_series, and recorder.find_events. "
    "Pass 'from'/'to' (e.g. a segment window from recorder.list_segments) to scope the activity ranking and "
    "the event count to that window instead of the whole session.",
    RPC_PARAMS(
        RPC_PARAM_REQ("session", "string", "Recording file path or id (from recorder.list_sessions)."),
        RPC_PARAM_OPT("objectCatalogCap", "number", "Max objects in the catalog before the tail is elided by activity. 0 = no cap."),
        RPC_PARAM_OPT("from", "number", "Window start timestamp (inclusive). Omit for the session start."),
        RPC_PARAM_OPT("to", "number", "Window end timestamp (inclusive). Omit for the session end.")
    ))
{
    FString Session;
    if (!Ctx.RequireString(TEXT("session"), Session)) return true;

    TUniquePtr<FRecorderQueryEngine> Engine;
    if (!ResolveEngine(Ctx, Session, Engine)) return true;

    const int32 Cap = Ctx.GetInt(TEXT("objectCatalogCap"), FRecorderQueryEngine::DefaultObjectCatalogCap);

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    double From = 0.0, To = 0.0;
    const bool bHasFrom = Payload.IsValid() && Payload->TryGetNumberField(TEXT("from"), From);
    const bool bHasTo = Payload.IsValid() && Payload->TryGetNumberField(TEXT("to"), To);

    Ctx.SendSuccess(Engine->Describe(Cap, bHasFrom, From, bHasTo, To));
    return true;
}

REGISTER_RPC_HANDLER("recorder.get_state", "recorder",
    "Get the as-of value of one recorded object's tags at a specific timestamp. For each requested tag the last "
    "value set at or before 't' is returned (carry-forward); a tag with no value at or before that time is marked "
    "noData. Call recorder.describe_session first to discover valid keys and tags.",
    RPC_PARAMS(
        RPC_PARAM_REQ("session", "string", "Recording file path or id."),
        RPC_PARAM_REQ("key", "string", "Object key, from the describe-session object catalog."),
        RPC_PARAM_OPT("tags", "array", "Tags to read. Omit for every tag recorded on the object."),
        RPC_PARAM_DEF("t", "number", "Timestamp (session seconds) to evaluate the as-of state at.", "0")
    ))
{
    FString Session;
    if (!Ctx.RequireString(TEXT("session"), Session)) return true;
    FString Key;
    if (!Ctx.RequireString(TEXT("key"), Key)) return true;

    TUniquePtr<FRecorderQueryEngine> Engine;
    if (!ResolveEngine(Ctx, Session, Engine)) return true;

    TArray<FString> Tags;
    if (const TArray<TSharedPtr<FJsonValue>>* TagsArr = Ctx.GetArray(TEXT("tags")))
    {
        for (const TSharedPtr<FJsonValue>& V : *TagsArr)
        {
            FString TagStr;
            if (V.IsValid() && V->TryGetString(TagStr) && !TagStr.IsEmpty())
            {
                Tags.Add(TagStr);
            }
        }
    }

    const double AtTs = Ctx.GetNumber(TEXT("t"), 0.0);
    Ctx.SendSuccess(Engine->GetState(Key, Tags, AtTs));
    return true;
}

REGISTER_RPC_HANDLER("recorder.summarize_change", "recorder",
    "The workhorse delta summary: as-of endpoints, net delta, min/max/mean of the magnitude, the timestamps where "
    "the extrema occurred (drill-down handles), and the change-point count for one (key, tag) over a time window. "
    "Vector kinds summarize their Euclidean magnitude.",
    RPC_PARAMS(
        RPC_PARAM_REQ("session", "string", "Recording file path or id."),
        RPC_PARAM_REQ("key", "string", "Object key."),
        RPC_PARAM_REQ("tag", "string", "Variable tag."),
        RPC_PARAM_DEF("from", "number", "Window start timestamp (inclusive).", "0"),
        RPC_PARAM_DEF("to", "number", "Window end timestamp (inclusive).", "0")
    ))
{
    FString Session;
    if (!Ctx.RequireString(TEXT("session"), Session)) return true;
    FString Key;
    if (!Ctx.RequireString(TEXT("key"), Key)) return true;
    FString Tag;
    if (!Ctx.RequireString(TEXT("tag"), Tag)) return true;

    TUniquePtr<FRecorderQueryEngine> Engine;
    if (!ResolveEngine(Ctx, Session, Engine)) return true;

    const double From = Ctx.GetNumber(TEXT("from"), 0.0);
    const double To = Ctx.GetNumber(TEXT("to"), 0.0);
    Ctx.SendSuccess(Engine->SummarizeChange(Key, Tag, From, To));
    return true;
}

REGISTER_RPC_HANDLER("recorder.get_series", "recorder",
    "Get the change-point series of one tag of one object over a time window, reduced to a bounded set of points. "
    "Reductions: 'downsample' (default) returns a peak-preserving point budget (first, last, and per-bucket "
    "extrema survive); 'diff' returns the raw change list paginated by 'cursor'. The result is hard-capped "
    "regardless of request — an over-broad window comes back truncated with the elided count in the envelope. "
    "Use recorder.summarize_change first to find the interesting window.",
    RPC_PARAMS(
        RPC_PARAM_REQ("session", "string", "Recording file path or id."),
        RPC_PARAM_REQ("key", "string", "Object key."),
        RPC_PARAM_REQ("tag", "string", "Variable tag."),
        RPC_PARAM_DEF("from", "number", "Window start timestamp (inclusive).", "0"),
        RPC_PARAM_DEF("to", "number", "Window end timestamp (inclusive).", "0"),
        RPC_PARAM_DEF("reduction", "string", "'downsample' (peak-preserving budget) or 'diff' (raw, paginated).", "downsample"),
        RPC_PARAM_DEF("maxPoints", "number", "Max points to return. Capped at the engine hard limit. 0 = default budget.", "0"),
        RPC_PARAM_OPT("cursor", "string", "Pagination cursor for 'diff'; pass the prior page's nextCursor.")
    ))
{
    FString Session;
    if (!Ctx.RequireString(TEXT("session"), Session)) return true;
    FString Key;
    if (!Ctx.RequireString(TEXT("key"), Key)) return true;
    FString Tag;
    if (!Ctx.RequireString(TEXT("tag"), Tag)) return true;

    TUniquePtr<FRecorderQueryEngine> Engine;
    if (!ResolveEngine(Ctx, Session, Engine)) return true;

    const double From = Ctx.GetNumber(TEXT("from"), 0.0);
    const double To = Ctx.GetNumber(TEXT("to"), 0.0);
    const FString Reduction = Ctx.GetString(TEXT("reduction"), TEXT("downsample"));
    const int32 MaxPoints = Ctx.GetInt(TEXT("maxPoints"), 0);
    const FString Cursor = Ctx.GetString(TEXT("cursor"));

    Ctx.SendSuccess(Engine->GetSeries(Key, Tag, From, To, Reduction, MaxPoints, Cursor));
    return true;
}

REGISTER_RPC_HANDLER("recorder.find_events", "recorder",
    "Find recorded events matching a filter — the 'locate the bad moment' tool. Filter by event name, minimum "
    "severity (Trace|Debug|Info|Warning|Error|Fatal), object key, and a time window; results are ordered by "
    "timestamp and carry the anchor timestamp plus the event's props; truncated results are paginated by 'cursor'. "
    "Feed an anchor timestamp back into "
    "recorder.get_state or recorder.summarize_change to inspect the surrounding state.",
    RPC_PARAMS(
        RPC_PARAM_REQ("session", "string", "Recording file path or id."),
        RPC_PARAM_OPT("name", "string", "Exact event name to match. Omit to match any name."),
        RPC_PARAM_OPT("severityMin", "string", "Minimum severity: Trace|Debug|Info|Warning|Error|Fatal. Omit for any."),
        RPC_PARAM_OPT("key", "string", "Restrict to events on one object key. Omit for every object plus global events."),
        RPC_PARAM_OPT("from", "number", "Window start timestamp (inclusive). Omit for no lower bound."),
        RPC_PARAM_OPT("to", "number", "Window end timestamp (inclusive). Omit for no upper bound."),
        RPC_PARAM_DEF("limit", "number", "Max events to return. Capped at the engine hard limit. 0 = default.", "0"),
        RPC_PARAM_OPT("cursor", "string", "Pagination cursor; pass the prior page's nextCursor.")
    ))
{
    FString Session;
    if (!Ctx.RequireString(TEXT("session"), Session)) return true;

    TUniquePtr<FRecorderQueryEngine> Engine;
    if (!ResolveEngine(Ctx, Session, Engine)) return true;

    const FString Name = Ctx.GetString(TEXT("name"));
    const FString Key = Ctx.GetString(TEXT("key"));

    bool bHasSeverityMin = false;
    int32 SeverityMin = 0;
    const FString SevName = Ctx.GetString(TEXT("severityMin"));
    if (!SevName.IsEmpty())
    {
        const int32 Level = RecorderQuery::SeverityLevelFromName(SevName);
        if (Level != INDEX_NONE)
        {
            bHasSeverityMin = true;
            SeverityMin = Level;
        }
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    double From = 0.0, To = 0.0;
    const bool bHasFrom = Payload.IsValid() && Payload->TryGetNumberField(TEXT("from"), From);
    const bool bHasTo = Payload.IsValid() && Payload->TryGetNumberField(TEXT("to"), To);
    const int32 Limit = Ctx.GetInt(TEXT("limit"), 0);
    const FString Cursor = Ctx.GetString(TEXT("cursor"));

    Ctx.SendSuccess(Engine->FindEvents(Name, bHasSeverityMin, SeverityMin, Key, bHasFrom, From, bHasTo, To, Limit, Cursor));
    return true;
}

REGISTER_RPC_HANDLER("recorder.list_segments", "recorder",
    "Pair the session's boundary events into named, typed segment windows. The segment types come from the "
    "host-registered rule set (RecorderSegmentRegistry); the built-in default provides editor_session, and the "
    "host project registers its own gameplay rules. Each segment carries a per-type 1-based index, tMin/tMax "
    "(feed them straight into the 'from'/'to' of recorder.describe_session, recorder.summarize_change, "
    "recorder.get_series, and recorder.find_events), the start event's props, and interior anchor events. An "
    "unpaired boundary degrades to a half-open segment flagged startOpen/endOpen instead of being dropped.",
    RPC_PARAMS(
        RPC_PARAM_REQ("session", "string", "Recording file path or id (from recorder.list_sessions).")
    ))
{
    FString Session;
    if (!Ctx.RequireString(TEXT("session"), Session)) return true;

    TUniquePtr<FRecorderQueryEngine> Engine;
    if (!ResolveEngine(Ctx, Session, Engine)) return true;

    Ctx.SendSuccess(Engine->ListSegments());
    return true;
}
