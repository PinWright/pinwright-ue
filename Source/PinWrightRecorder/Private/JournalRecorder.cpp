// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "JournalRecorder.h"
#include "JournalSession.h"

#include "Containers/Queue.h"
#include "HAL/PlatformTime.h"
#include "UObject/Object.h"

std::atomic<bool> FJournalRecorder::GRecording{ false };
TUniquePtr<FJournalSession> FJournalRecorder::GSession;

namespace
{
    /** Lock-free MPSC queue: any producer thread enqueues; the game thread drains. */
    TQueue<FRecordMsg, EQueueMode::Mpsc> GQueue;

    /** Per-thread domain context inherited by subsequent logs on the same thread. */
    struct FThreadStamp
    {
        EJournalDomain Domain = EJournalDomain::None;
        double DomainTime = 0.0;
        int64 DomainFrame = -1;
    };
    thread_local FThreadStamp GThreadStamp;

    /** Wall-clock seconds for the current call site (primary time axis). */
    FORCEINLINE double NowSeconds()
    {
        return FPlatformTime::Cycles64() * FPlatformTime::GetSecondsPerCycle64();
    }
}

void FJournalRecorder::BeginSession(const FString& Label, int32 RetentionCap, const FString& RecordingsSubdir)
{
    if (GRecording.load(std::memory_order_relaxed))
    {
        return;
    }

    GSession = FJournalSession::Create(Label, RetentionCap, RecordingsSubdir);
    if (!GSession.IsValid())
    {
        return;
    }

    // Publish the gate after the session is fully constructed so producers never enqueue
    // against a half-built session.
    GRecording.store(true, std::memory_order_release);
}

void FJournalRecorder::EndSession()
{
    if (!GSession.IsValid())
    {
        return;
    }

    // Stop producers first, then drain whatever they enqueued before the gate flipped.
    GRecording.store(false, std::memory_order_release);
    DrainAndFlush();

    GSession->Flush();
    GSession.Reset();
}

void FJournalRecorder::DrainAndFlush()
{
    if (!GSession.IsValid())
    {
        // Still drain so the queue does not grow unbounded if a stray message slipped in.
        FRecordMsg Discard;
        while (GQueue.Dequeue(Discard)) {}
        return;
    }

    FRecordMsg Msg;
    while (GQueue.Dequeue(Msg))
    {
        switch (Msg.MsgType)
        {
        case EJournalMsgType::RegisterObject:
            GSession->UpsertObject(Msg.Key, Msg.Label, FString(), FString(), NAME_None, Msg.Ts);
            break;

        case EJournalMsgType::Value:
            GSession->TryAppendValue(Msg.Ts, Msg.Domain, Msg.DomainTime, Msg.DomainFrame, Msg.Key, Msg.Tag, Msg.Value);
            break;

        case EJournalMsgType::Event:
            GSession->AppendEvent(Msg.Ts, Msg.Domain, Msg.Key, Msg.Tag, Msg.Severity, Msg.Props);
            break;
        }
    }

    GSession->Flush();
}

FJournalLiveTail* FJournalRecorder::GetLiveTail()
{
    return GSession.IsValid() ? &GSession->GetLiveTail() : nullptr;
}

void FJournalRecorder::StampDomain(EJournalDomain Domain, double DomainTime, int64 DomainFrame)
{
    GThreadStamp.Domain = Domain;
    GThreadStamp.DomainTime = DomainTime;
    GThreadStamp.DomainFrame = DomainFrame;
}

void FJournalRecorder::RegisterObject(FName Key, const FString& Label)
{
    if (!IsRecording() || Key.IsNone())
    {
        return;
    }

    FRecordMsg Msg;
    Msg.MsgType = EJournalMsgType::RegisterObject;
    Msg.Ts = NowSeconds();
    Msg.Key = Key;
    Msg.Label = Label;
    GQueue.Enqueue(MoveTemp(Msg));
}

void FJournalRecorder::RegisterObject(const UObject* Object, const FString& Label)
{
    RegisterObject(KeyFor(Object), Label.IsEmpty() && Object ? Object->GetName() : Label);
}

FName FJournalRecorder::KeyFor(const UObject* Object)
{
    return Object ? Object->GetFName() : NAME_None;
}

FName FJournalRecorder::ResolveKey(const UObject* Object)
{
    const FName Key = KeyFor(Object);
    if (Object)
    {
        // Auto-register the catalog entry on first sight; UpsertObject de-dupes downstream.
        RegisterObject(Key, Object->GetName());
    }
    return Key;
}

void FJournalRecorder::Append(FName Key, FName Tag, const FRecordedValue& Value)
{
    if (!IsRecording())
    {
        return;
    }

    FRecordMsg Msg;
    Msg.MsgType = EJournalMsgType::Value;
    Msg.Ts = NowSeconds();
    Msg.Domain = GThreadStamp.Domain;
    Msg.DomainTime = GThreadStamp.DomainTime;
    Msg.DomainFrame = GThreadStamp.DomainFrame;
    Msg.Key = Key;
    Msg.Tag = Tag;
    Msg.Value = Value;
    GQueue.Enqueue(MoveTemp(Msg));
}

void FJournalRecorder::LogEvent(FName Key, FName Name, TArray<TPair<FName, FRecordedValue>> Props, EJournalSeverity Severity)
{
    if (!IsRecording())
    {
        return;
    }

    FRecordMsg Msg;
    Msg.MsgType = EJournalMsgType::Event;
    Msg.Ts = NowSeconds();
    Msg.Domain = GThreadStamp.Domain;
    Msg.DomainTime = GThreadStamp.DomainTime;
    Msg.DomainFrame = GThreadStamp.DomainFrame;
    Msg.Key = Key;
    Msg.Tag = Name;
    Msg.Severity = Severity;
    Msg.Props = MoveTemp(Props);
    GQueue.Enqueue(MoveTemp(Msg));
}
