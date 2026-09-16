// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "JournalTypes.h"

class IFileHandle;

/** Catalog entry for one recorded object, written once on first sight. */
struct FObjectRecord
{
    FName Key;
    FString Label;
    FString Path;
    FString Type;
    FName ParentKey;
    double TFirst = 0.0;
};

/** Per-tag manifest entry, written once on first sight of a tag. */
struct FVariableManifest
{
    FName Tag;
    EJournalKind Kind = EJournalKind::Float;
    FString Unit;
    double Epsilon = 0.0;
    EJournalDomain Domain = EJournalDomain::None;
    double TFirst = 0.0;
};

/**
 * Hand-rolled NDJSON writer for a recording session. Each line is built directly into a
 * FString with invariant-culture numbers and escaped strings (UTF-8, no BOM), buffered in
 * memory, and handed to the OS in one plain write() per Flush() — never a device flush
 * (see Flush() for the durability contract). Lines match the schema the gateway's query
 * loader parses. Ported from the Unity recorder's NdjsonSessionWriter.
 */
class FNdjsonSessionWriter
{
public:
    /** Opens (truncates) the session file and prepares the write handle. Validity via IsValid(). */
    explicit FNdjsonSessionWriter(const FString& FilePath);
    ~FNdjsonSessionWriter();

    bool IsValid() const { return Handle.IsValid(); }

    void WriteHeader(const FString& SessionId, int32 Fmt, const FString& EngineVersion, const FString& StartUtcIso, double T0);
    void WriteObject(const FObjectRecord& Record);
    void WriteVariable(const FVariableManifest& Manifest);
    void WriteValue(double Ts, EJournalDomain Domain, double DomainTime, int64 DomainFrame, FName Key, FName Tag, const FRecordedValue& Value);
    void WriteEvent(int32 EventId, double Ts, EJournalDomain Domain, FName Key, FName Name, EJournalSeverity Severity, const TArray<TPair<FName, FRecordedValue>>& Props);

    /**
     * Hand the lines buffered since the last Flush() to the OS in a single write(). This is a
     * page-cache write only — it makes the bytes visible to concurrent readers and durable
     * across an editor crash, but deliberately performs NO device flush. IFileHandle::Flush()
     * is FlushFileBuffers on Windows (a full physical-media sync): calling it once per
     * game-thread drain tick stalled PIE for 270-400 ms whenever the OS/drive had dirty data
     * queued (~1-2 times per second), so the journal never syncs the device; power-loss
     * durability is out of scope for a debug journal.
     */
    void Flush();

private:
    void AppendString(FString& Out, const FString& Name, const FString& Value) const;
    void AppendRaw(FString& Out, const FString& Name, const FString& RawJson) const;
    void AppendEscapedString(FString& Out, const FString& Value) const;
    void AppendValuePayload(FString& Out, const FRecordedValue& Value) const;
    void AppendPropsObject(FString& Out, const TArray<TPair<FName, FRecordedValue>>& Props) const;
    void WriteLine(const FString& Line);

    TUniquePtr<IFileHandle> Handle;

    /** UTF-8 line bytes accumulated since the last Flush(); reused between drains (Reset keeps capacity). */
    TArray<uint8> PendingBytes;
};
