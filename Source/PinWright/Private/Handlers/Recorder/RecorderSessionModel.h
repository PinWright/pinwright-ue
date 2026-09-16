// Copyright (c) 2026 Alexander Penkin. MIT License.

// In-memory model of one journal recording session, rebuilt from an NDJSON file.
//
// The recorder writes change-point-compressed NDJSON (schema documented in the wave
// plan's Context section); this model is the parsed, query-ready form: an object
// catalog, a per-tag variable manifest, per-(key,tag) series sorted by wall-clock
// timestamp, and an event list. The primary axis is a double timestamp `Ts`
// (seconds); `DomainFrame` is carried as a secondary integer drill-down handle.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace RecorderModel
{

// Variant discriminator for a recorded value. Mirrors the writer's `kind` strings.
enum class EValueKind : uint8
{
    Float,
    Int,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Rotator,
    Enum,
    String
};

// Tagged value for one recorded sample. Up to four doubles cover every numeric /
// vector kind; S carries string payloads (and the enum member name).
struct FValuePoint
{
    EValueKind Kind = EValueKind::Float;
    double F0 = 0.0;
    double F1 = 0.0;
    double F2 = 0.0;
    double F3 = 0.0;
    FString S;

    // Scalar magnitude: the value itself for scalars, Euclidean norm for vector
    // kinds, NaN for strings (so string series never participate in extrema math).
    double Magnitude() const;

    // True when this value differs from Other by more than Epsilon. Vector/quat/
    // rotator kinds compare the Euclidean norm of the component delta; scalars
    // compare absolute delta; string/enum compare for any difference.
    bool SignificantlyDiffers(const FValuePoint& Other, double Epsilon) const;
};

// One emitted change in a (key, tag) series: the wall-clock timestamp it occurred,
// the integer domain frame (secondary handle), and the value it changed to. Series
// are stored sorted by Ts for as-of (last Ts <= N) lookups.
struct FChangePoint
{
    double Ts = 0.0;
    int64 DomainFrame = -1;
    FValuePoint Value;
};

// One catalog entry: an instrumented object that appeared in the session.
struct FObjectRecord
{
    FString Key;
    FString Label;
    FString Path;
    FString Type;
    FString ParentKey;
    double TsFirst = 0.0;
    // Number of change points logged against this object's series; drives the
    // describe_session activity ranking used to cap the catalog.
    int64 Activity = 0;
};

// One per-tag manifest entry.
struct FVariableManifest
{
    FString Tag;
    EValueKind Kind = EValueKind::Float;
    FString Unit;
    bool bHasEpsilon = false;
    double Epsilon = 0.0;
    FString Domain;
    double TsFirst = 0.0;
    // True when the tag's value kind drifted across rows (rare; instrumentation bug signal).
    bool bMixed = false;
};

// One instantaneous event. Key is empty for global (object-less) events.
struct FEventRecord
{
    int64 EventId = 0;
    double Ts = 0.0;
    FString Domain;
    FString Key;
    FString Name;
    // Severity name as written (e.g. "Info", "Warning"); empty when unset.
    FString Severity;
    // Raw props sub-object preserved verbatim for echo-back.
    TSharedPtr<FJsonObject> Props;
};

// The parsed session. Owns every record; query algorithms read it but never mutate it.
struct FSessionModel
{
    FString SessionId;
    FString Label;
    FString Engine;
    FString StartUtc;
    double T0 = 0.0;
    FString TimeAxis;
    TArray<FString> Domains;

    TMap<FString, FObjectRecord> Objects;
    TMap<FString, FVariableManifest> Variables;
    TMap<TPair<FString, FString>, TArray<FChangePoint>> Series;
    TArray<FEventRecord> Events;

    double MinTs = TNumericLimits<double>::Max();
    double MaxTs = TNumericLimits<double>::Lowest();

    bool HasTimeRange() const { return MinTs <= MaxTs; }

    // Lookup helpers used by the query engine.
    const TArray<FChangePoint>* FindSeries(const FString& Key, const FString& Tag) const
    {
        return Series.Find(TPair<FString, FString>(Key, Tag));
    }

    EValueKind KindOf(const FString& Tag) const
    {
        if (const FVariableManifest* V = Variables.Find(Tag))
        {
            return V->Kind;
        }
        return EValueKind::Float;
    }

    FString UnitOf(const FString& Tag) const
    {
        if (const FVariableManifest* V = Variables.Find(Tag))
        {
            return V->Unit;
        }
        return FString();
    }

    // Returns the tag's epsilon, or DefaultEpsilon when the manifest declared none.
    double EpsilonOf(const FString& Tag, double DefaultEpsilon) const
    {
        if (const FVariableManifest* V = Variables.Find(Tag))
        {
            return V->bHasEpsilon ? V->Epsilon : DefaultEpsilon;
        }
        return DefaultEpsilon;
    }

    // Distinct tags recorded against an object key, in series-insertion order.
    TArray<FString> TagsForKey(const FString& Key) const
    {
        TArray<FString> Tags;
        for (const TPair<TPair<FString, FString>, TArray<FChangePoint>>& Entry : Series)
        {
            if (Entry.Key.Key == Key)
            {
                Tags.AddUnique(Entry.Key.Value);
            }
        }
        return Tags;
    }
};

// String <-> kind conversion shared by the loader and the result encoder.
EValueKind KindFromString(const FString& In);
const TCHAR* KindToString(EValueKind Kind);

} // namespace RecorderModel
