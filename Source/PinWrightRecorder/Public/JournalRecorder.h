// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "JournalTypes.h"
#if __has_include("Templates/Requires.h")
#include "Templates/Requires.h"  // UE_REQUIRES moved here in 5.5; on 5.4 it comes from CoreMinimal's UnrealTypeTraits.h
#endif
#include "Templates/IsEnum.h"
#include "UObject/Class.h"

#include <atomic>

class UObject;
class FJournalSession;
class FJournalLiveTail;

/**
 * Engine-agnostic static entry point for the editor-only debug journal recorder. Gameplay
 * code calls LogVariable / LogEvent during a PIE session; producers (any thread) enqueue
 * lock-free and a single game-thread consumer (DrainAndFlush) does change-point compression
 * and NDJSON file I/O. Every call is a cheap no-op while no session is active.
 *
 * Standalone editor-side journal recorder module with no dependencies on the host project;
 * host-project modules may link against it.
 */
class PINWRIGHTRECORDER_API FJournalRecorder
{
public:
    // --- Recording gate ---------------------------------------------------

    /** True while a session is open. Relaxed atomic load — safe and cheap from any thread. */
    static FORCEINLINE bool IsRecording()
    {
        return GRecording.load(std::memory_order_relaxed);
    }

    // --- Lifecycle (game thread) ------------------------------------------

    /**
     * Open a new recording session; no-ops if one is already open. Creates the file (keeping the
     * newest RetentionCap session files under Saved/<RecordingsSubdir>) and writes the header.
     */
    static void BeginSession(const FString& Label, int32 RetentionCap, const FString& RecordingsSubdir);

    /** Drain remaining messages, flush, close, and clear the active session. */
    static void EndSession();

    /** Dequeue all pending producer messages, run change-point compression, and write NDJSON. Game thread only. */
    static void DrainAndFlush();

    // --- Live journal tail (game thread) --------------------------------

    /**
     * Active session's live in-memory tail, or nullptr when no session is open. Game-thread only;
     * the returned pointer is owned by the session and invalidated by EndSession — re-fetch per use.
     */
    static FJournalLiveTail* GetLiveTail();

    // --- Per-thread domain stamp ------------------------------------------

    /**
     * Stamp the calling thread's domain context. Subsequent LogVariable/LogEvent calls on the
     * same thread inherit this domain/time/frame until re-stamped. Thread-local, so the async
     * physics thread and the game thread can carry independent stamps.
     */
    static void StampDomain(EJournalDomain Domain, double DomainTime, int64 DomainFrame);

    // --- Object catalog ---------------------------------------------------

    /** Register a catalog entry under a caller-supplied key (e.g. a per-life GUID FName). */
    static void RegisterObject(FName Key, const FString& Label);

    /** Register a catalog entry derived from a UObject (uses its name as the label). */
    static void RegisterObject(const UObject* Object, const FString& Label);

    /** Derive a stable per-object key for the string-keyed series. */
    static FName KeyFor(const UObject* Object);

    // --- Variable logging (key-by-FName) ----------------------------------

    static void LogVariable(FName Key, FName Tag, float Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, double Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, int32 Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, int64 Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, bool Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, const FVector2D& Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, const FVector& Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, const FVector4& Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, const FQuat& Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, const FRotator& Value) { Append(Key, Tag, FRecordedValue::From(Value)); }
    static void LogVariable(FName Key, FName Tag, const FString& Value) { Append(Key, Tag, FRecordedValue::From(Value)); }

    /**
     * Enum overload: records the underlying integer plus the member name. The enum must be a
     * reflected UENUM (the only enums logged through the facade) — StaticEnum<TEnum>() resolves
     * the member name from its codegen specialization.
     */
    template <typename TEnum UE_REQUIRES(TIsEnum<TEnum>::Value)>
    static void LogVariable(FName Key, FName Tag, TEnum Value)
    {
        const int64 Underlying = static_cast<int64>(Value);
        const UEnum* EnumType = StaticEnum<TEnum>();
        const FString MemberName = EnumType ? EnumType->GetNameStringByValue(Underlying) : LexToString(Underlying);
        Append(Key, Tag, FRecordedValue::FromEnum(Underlying, MemberName));
    }

    // --- Variable logging (key-by-UObject, auto-registers on first sight) ---

    template <typename T>
    static void LogVariable(const UObject* Object, FName Tag, T&& Value)
    {
        LogVariable(ResolveKey(Object), Tag, Forward<T>(Value));
    }

    // --- Event logging ----------------------------------------------------

    /** Object-keyed event. */
    static void LogEvent(FName Key, FName Name, TArray<TPair<FName, FRecordedValue>> Props, EJournalSeverity Severity = EJournalSeverity::Info);

    /** Global (object-less) event. */
    static void LogEvent(FName Name, TArray<TPair<FName, FRecordedValue>> Props, EJournalSeverity Severity = EJournalSeverity::Info)
    {
        LogEvent(NAME_None, Name, MoveTemp(Props), Severity);
    }

private:
    /** Build the message, stamp wall-clock + the thread's domain context, and enqueue. */
    static void Append(FName Key, FName Tag, const FRecordedValue& Value);

    /** Resolve (and auto-register) a key for a UObject-keyed log. */
    static FName ResolveKey(const UObject* Object);

    /** Set by BeginSession / cleared by EndSession; read by IsRecording from any thread. */
    static std::atomic<bool> GRecording;

    /** Active session (game-thread-owned; nulled outside a session). */
    static TUniquePtr<FJournalSession> GSession;
};

/**
 * Guard macro so the value expression is not evaluated when recording is off (the relaxed
 * atomic load is the only cost in the common no-record case).
 */
#define PW_JOURNAL_LOG(KeyOrObj, Tag, Value) \
    do { if (FJournalRecorder::IsRecording()) FJournalRecorder::LogVariable((KeyOrObj), (Tag), (Value)); } while (0)
