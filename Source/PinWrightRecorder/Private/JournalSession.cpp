// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "JournalSession.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"

namespace
{
    /** NDJSON line-format version emitted in the header. */
    constexpr int32 GJournalFmt = 1;

    /** Fallback subdirectory under Saved when the configured directory is empty. */
    const TCHAR* GDefaultRecordingsSubdir = TEXT("PinWright/Recordings");

    /** Absolute session-file directory: the configured subdir (or the default) under the project's Saved folder. */
    FString RecordingsDir(const FString& RecordingsSubdir)
    {
        const FString Subdir = RecordingsSubdir.IsEmpty() ? FString(GDefaultRecordingsSubdir) : RecordingsSubdir;
        return FPaths::ProjectSavedDir() / Subdir;
    }

    /** Delete the oldest session files once the directory exceeds the retention cap. Best-effort. */
    void EnforceRetentionCap(const FString& Dir, int32 RetentionCap)
    {
        if (RetentionCap <= 0 || !IFileManager::Get().DirectoryExists(*Dir))
        {
            return;
        }

        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *(Dir / TEXT("session-*.ndjson")), true, false);
        if (Files.Num() < RetentionCap)
        {
            return;
        }

        // Sort newest-first by timestamp, keep RetentionCap-1, delete the rest (about to add one more).
        Files.Sort([&Dir](const FString& A, const FString& B)
        {
            return IFileManager::Get().GetTimeStamp(*(Dir / A)) > IFileManager::Get().GetTimeStamp(*(Dir / B));
        });

        for (int32 Index = RetentionCap - 1; Index < Files.Num(); ++Index)
        {
            IFileManager::Get().Delete(*(Dir / Files[Index]), false, false, true);
        }
    }
}

TUniquePtr<FJournalSession> FJournalSession::Create(const FString& Label, int32 RetentionCap, const FString& RecordingsSubdir)
{
    const FDateTime StartUtc = FDateTime::UtcNow();
    const FString SessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString FileName = FString::Printf(TEXT("session-%s-%s.ndjson"), *StartUtc.ToString(TEXT("%Y%m%d-%H%M%S")), *SessionId);
    const FString Dir = RecordingsDir(RecordingsSubdir);
    const FString FilePath = Dir / FileName;

    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);
    EnforceRetentionCap(Dir, RetentionCap);

    TUniquePtr<FNdjsonSessionWriter> Writer = MakeUnique<FNdjsonSessionWriter>(FilePath);
    if (!Writer->IsValid())
    {
        return nullptr;
    }

    // Session-start wall-clock seconds (same clock the producers stamp values against).
    const double T0 = FPlatformTime::Cycles64() * FPlatformTime::GetSecondsPerCycle64();
    const FEngineVersion& EngineVer = FEngineVersion::Current();
    const FString EngineVersion = FString::Printf(TEXT("%d.%d.%d"), EngineVer.GetMajor(), EngineVer.GetMinor(), EngineVer.GetPatch());
    Writer->WriteHeader(SessionId, GJournalFmt, EngineVersion, StartUtc.ToIso8601(), T0);

    TUniquePtr<FJournalSession> Session = MakeUnique<FJournalSession>();
    Session->Writer = MoveTemp(Writer);
    return Session;
}

void FJournalSession::UpsertObject(FName Key, const FString& Label, const FString& Path, const FString& Type, FName ParentKey, double Ts)
{
    if (Key.IsNone() || KnownObjects.Contains(Key))
    {
        return;
    }

    KnownObjects.Add(Key);
    if (Writer)
    {
        FObjectRecord Record;
        Record.Key = Key;
        Record.Label = Label;
        Record.Path = Path;
        Record.Type = Type;
        Record.ParentKey = ParentKey;
        Record.TFirst = Ts;
        Writer->WriteObject(Record);
    }
}

void FJournalSession::UpsertVariable(FName Tag, EJournalKind Kind, double Epsilon, EJournalDomain Domain, double Ts)
{
    if (Variables.Contains(Tag))
    {
        return;
    }

    FVariableManifest Manifest;
    Manifest.Tag = Tag;
    Manifest.Kind = Kind;
    Manifest.Epsilon = Epsilon;
    Manifest.Domain = Domain;
    Manifest.TFirst = Ts;
    Variables.Add(Tag, Manifest);

    if (Writer)
    {
        Writer->WriteVariable(Manifest);
    }
}

void FJournalSession::TryAppendValue(double Ts, EJournalDomain Domain, double DomainTime, int64 DomainFrame, FName Key, FName Tag, const FRecordedValue& Value)
{
    UpsertVariable(Tag, Value.Kind, DefaultEpsilon(Value.Kind), Domain, Ts);

    const FSeriesKey SeriesKey{ Key, Tag };
    if (const FRecordedValue* Last = LastBySeries.Find(SeriesKey))
    {
        const double Epsilon = Variables.Contains(Tag) ? Variables[Tag].Epsilon : 0.0;
        if (!Value.SignificantlyDiffers(*Last, Epsilon))
        {
            return;
        }
    }

    LastBySeries.Add(SeriesKey, Value);
    if (Writer)
    {
        Writer->WriteValue(Ts, Domain, DomainTime, DomainFrame, Key, Tag, Value);
    }

    // Feed the live tail in lockstep with the NDJSON change point (only on real changes).
    LiveTail.PushVariable(Ts, Domain, Key, Tag, Value);
}

void FJournalSession::AppendEvent(double Ts, EJournalDomain Domain, FName Key, FName Name, EJournalSeverity Severity, const TArray<TPair<FName, FRecordedValue>>& Props)
{
    const int32 EventId = NextEventId++;
    if (Writer)
    {
        Writer->WriteEvent(EventId, Ts, Domain, Key, Name, Severity, Props);
    }

    // Feed the live tail in lockstep with the NDJSON event write.
    LiveTail.PushEvent(Ts, Domain, Key, Name, Severity, Props);
}

double FJournalSession::DefaultEpsilon(EJournalKind Kind)
{
    switch (Kind)
    {
    case EJournalKind::Float:
    case EJournalKind::Vec2:
    case EJournalKind::Vec3:
    case EJournalKind::Vec4:
    case EJournalKind::Quat:
    case EJournalKind::Rotator:
        return 0.01;
    default:
        return 0.0;
    }
}
