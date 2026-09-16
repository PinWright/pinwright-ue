// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "RecorderSessionLoader.h"

#include "Utils/JsonUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace RecorderModel;

namespace
{

// One cached parse keyed by the file's modification time.
struct FCacheEntry
{
    FDateTime ModTime;
    TSharedPtr<FSessionModel> Model;
};

TMap<FString, FCacheEntry>& Cache()
{
    static TMap<FString, FCacheEntry> Instance;
    return Instance;
}

// Decode the `v`/`s` payload of a `val` line into a FValuePoint for the given kind.
FValuePoint DecodeValue(const TSharedPtr<FJsonObject>& Line, EValueKind Kind)
{
    FValuePoint Out;
    Out.Kind = Kind;

    switch (Kind)
    {
    case EValueKind::String:
        Line->TryGetStringField(TEXT("s"), Out.S);
        break;

    case EValueKind::Enum:
        // Enum carries the integer ordinal in `v` and the member name in `s`.
        Line->TryGetNumberField(TEXT("v"), Out.F0);
        Line->TryGetStringField(TEXT("s"), Out.S);
        break;

    case EValueKind::Float:
    case EValueKind::Int:
    case EValueKind::Bool:
        Line->TryGetNumberField(TEXT("v"), Out.F0);
        break;

    default:
    {
        // Vector kinds: `v` is a JSON array of components.
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Line->TryGetArrayField(TEXT("v"), Arr) && Arr)
        {
            const int32 N = Arr->Num();
            if (N > 0) Out.F0 = (*Arr)[0]->AsNumber();
            if (N > 1) Out.F1 = (*Arr)[1]->AsNumber();
            if (N > 2) Out.F2 = (*Arr)[2]->AsNumber();
            if (N > 3) Out.F3 = (*Arr)[3]->AsNumber();
        }
        break;
    }
    }
    return Out;
}

void TrackTs(FSessionModel& Model, double Ts)
{
    if (Ts < Model.MinTs) Model.MinTs = Ts;
    if (Ts > Model.MaxTs) Model.MaxTs = Ts;
}

void ApplyHeader(FSessionModel& Model, const TSharedPtr<FJsonObject>& Line)
{
    Line->TryGetStringField(TEXT("session"), Model.SessionId);
    Line->TryGetStringField(TEXT("engine"), Model.Engine);
    Line->TryGetStringField(TEXT("startUtc"), Model.StartUtc);
    Line->TryGetNumberField(TEXT("t0"), Model.T0);
    Line->TryGetStringField(TEXT("axis"), Model.TimeAxis);

    const TArray<TSharedPtr<FJsonValue>>* Domains = nullptr;
    if (Line->TryGetArrayField(TEXT("domains"), Domains) && Domains)
    {
        for (const TSharedPtr<FJsonValue>& V : *Domains)
        {
            Model.Domains.Add(V->AsString());
        }
    }
}

void ApplyObject(FSessionModel& Model, const TSharedPtr<FJsonObject>& Line)
{
    FString Key;
    if (!Line->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
    {
        return;
    }
    if (Model.Objects.Contains(Key))
    {
        return;
    }
    FObjectRecord Record;
    Record.Key = Key;
    Line->TryGetStringField(TEXT("label"), Record.Label);
    Line->TryGetStringField(TEXT("path"), Record.Path);
    Line->TryGetStringField(TEXT("type"), Record.Type);
    Line->TryGetStringField(TEXT("parent"), Record.ParentKey);
    Line->TryGetNumberField(TEXT("tFirst"), Record.TsFirst);
    Model.Objects.Add(Key, MoveTemp(Record));
}

void ApplyVariable(FSessionModel& Model, const TSharedPtr<FJsonObject>& Line)
{
    FString Tag;
    if (!Line->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
    {
        return;
    }
    if (Model.Variables.Contains(Tag))
    {
        return;
    }
    FVariableManifest Manifest;
    Manifest.Tag = Tag;
    FString KindStr;
    Line->TryGetStringField(TEXT("kind"), KindStr);
    Manifest.Kind = KindFromString(KindStr);
    Line->TryGetStringField(TEXT("unit"), Manifest.Unit);
    Line->TryGetStringField(TEXT("domain"), Manifest.Domain);
    Line->TryGetNumberField(TEXT("tFirst"), Manifest.TsFirst);
    double Eps = 0.0;
    if (Line->TryGetNumberField(TEXT("eps"), Eps))
    {
        Manifest.bHasEpsilon = true;
        Manifest.Epsilon = Eps;
    }
    Model.Variables.Add(Tag, MoveTemp(Manifest));
}

void ApplyValue(FSessionModel& Model, const TSharedPtr<FJsonObject>& Line)
{
    FString Key, Tag, KindStr;
    Line->TryGetStringField(TEXT("key"), Key);
    if (!Line->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
    {
        return;
    }
    Line->TryGetStringField(TEXT("kind"), KindStr);
    const EValueKind Kind = KindFromString(KindStr);

    double Ts = 0.0;
    Line->TryGetNumberField(TEXT("t"), Ts);

    FChangePoint Point;
    Point.Ts = Ts;
    int64 Df = -1;
    if (Line->TryGetNumberField(TEXT("df"), Df))
    {
        Point.DomainFrame = Df;
    }
    Point.Value = DecodeValue(Line, Kind);

    // A val line may precede its var line if the file is reordered; record the kind
    // so describe_session still reports a manifest for it. A later kind that
    // disagrees marks the tag mixed.
    if (FVariableManifest* Existing = Model.Variables.Find(Tag))
    {
        if (Existing->Kind != Kind)
        {
            Existing->bMixed = true;
        }
    }

    const TPair<FString, FString> SeriesKey(Key, Tag);
    TArray<FChangePoint>& Series = Model.Series.FindOrAdd(SeriesKey);
    Series.Add(MoveTemp(Point));

    if (FObjectRecord* Obj = Model.Objects.Find(Key))
    {
        ++Obj->Activity;
    }

    TrackTs(Model, Ts);
}

void ApplyEvent(FSessionModel& Model, const TSharedPtr<FJsonObject>& Line)
{
    FEventRecord Record;
    int64 Id = 0;
    Line->TryGetNumberField(TEXT("id"), Id);
    Record.EventId = Id;
    Line->TryGetNumberField(TEXT("t"), Record.Ts);
    Line->TryGetStringField(TEXT("dom"), Record.Domain);
    Line->TryGetStringField(TEXT("key"), Record.Key);
    Line->TryGetStringField(TEXT("name"), Record.Name);

    // `sev` may arrive as a severity name (string) or an integer level; normalize
    // both to the name string the query echoes back.
    FString SevStr;
    if (Line->TryGetStringField(TEXT("sev"), SevStr))
    {
        Record.Severity = SevStr;
    }
    else
    {
        double SevNum = 0.0;
        if (Line->TryGetNumberField(TEXT("sev"), SevNum))
        {
            Record.Severity = FString::FromInt((int32)SevNum);
        }
    }

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Line->TryGetObjectField(TEXT("props"), PropsObj) && PropsObj)
    {
        Record.Props = *PropsObj;
    }

    Model.Events.Add(MoveTemp(Record));
    TrackTs(Model, Record.Ts);
}

} // namespace

namespace RecorderSessionLoader
{

TSharedPtr<FSessionModel> ParseContent(const FString& Content)
{
    TSharedRef<FSessionModel> Model = MakeShared<FSessionModel>();

    const TArray<FString> Lines = ExtractTopLevelJsonObjects(Content);
    bool bHasHeader = false;

    for (const FString& Raw : Lines)
    {
        TSharedPtr<FJsonObject> Line;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        if (!FJsonSerializer::Deserialize(Reader, Line) || !Line.IsValid())
        {
            // Crash-truncated tail or corrupt line: stop cleanly, keep what parsed.
            break;
        }

        FString Kind;
        if (!Line->TryGetStringField(TEXT("k"), Kind))
        {
            continue;
        }

        if (Kind == TEXT("header"))
        {
            ApplyHeader(*Model, Line);
            bHasHeader = true;
        }
        else if (Kind == TEXT("obj"))
        {
            ApplyObject(*Model, Line);
        }
        else if (Kind == TEXT("var"))
        {
            ApplyVariable(*Model, Line);
        }
        else if (Kind == TEXT("val"))
        {
            ApplyValue(*Model, Line);
        }
        else if (Kind == TEXT("evt"))
        {
            ApplyEvent(*Model, Line);
        }
    }

    if (!bHasHeader && Lines.Num() == 0)
    {
        return nullptr;
    }

    // Series are appended in file order; sort by timestamp so the as-of binary
    // searches hold even if the writer interleaved threads out of order.
    for (TPair<TPair<FString, FString>, TArray<FChangePoint>>& Entry : Model->Series)
    {
        Entry.Value.StableSort([](const FChangePoint& A, const FChangePoint& B)
        {
            return A.Ts < B.Ts;
        });
    }

    return Model;
}

TSharedPtr<FSessionModel> Load(const FString& AbsolutePath, FString& OutError)
{
    if (!FPaths::FileExists(AbsolutePath))
    {
        OutError = FString::Printf(TEXT("Recording session not found: '%s'."), *AbsolutePath);
        return nullptr;
    }

    const FDateTime ModTime = IFileManager::Get().GetTimeStamp(*AbsolutePath);
    if (const FCacheEntry* Cached = Cache().Find(AbsolutePath))
    {
        if (Cached->ModTime == ModTime && Cached->Model.IsValid())
        {
            return Cached->Model;
        }
    }

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *AbsolutePath))
    {
        OutError = FString::Printf(TEXT("Failed to read recording session: '%s'."), *AbsolutePath);
        return nullptr;
    }

    TSharedPtr<FSessionModel> Model = ParseContent(Content);
    if (!Model.IsValid())
    {
        OutError = FString::Printf(TEXT("Recording session is empty or malformed: '%s'."), *AbsolutePath);
        return nullptr;
    }
    if (Model->SessionId.IsEmpty())
    {
        Model->SessionId = FPaths::GetBaseFilename(AbsolutePath);
    }

    FCacheEntry Entry;
    Entry.ModTime = ModTime;
    Entry.Model = Model;
    Cache().Add(AbsolutePath, MoveTemp(Entry));
    return Model;
}

void ClearCache()
{
    Cache().Empty();
}

} // namespace RecorderSessionLoader
