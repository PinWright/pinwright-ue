// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "State/ClientActivity.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Math/NumericLimits.h"

namespace ClientActivity
{
namespace
{
    struct FClientRecord
    {
        FString LastMethod;
        double LastSeenSeconds = 0.0;
    };

    // One mutex for the whole ledger: the I/O thread adds one map entry per
    // request, the game thread touches both maps once per dispatch, and a handler
    // reads them once. Contention is nil and no critical section outlives a few
    // FString copies.
    FCriticalSection GMutex;

    // RequestId -> ClientId, parked by the I/O thread and consumed by the game
    // thread one dispatch later. Bounded because the two halves are not paired by
    // construction: a request rejected after the id is parked, or one whose
    // dispatch never runs, would otherwise leak an entry for the process
    // lifetime. Insertion order is kept so the oldest is dropped first; a dropped
    // entry costs that one request its identity and nothing else.
    TMap<FString, FString> GPendingClients;
    TArray<FString> GPendingOrder;
    constexpr int32 GMaxPendingClients = 256;

    // ClientId -> its last dispatch. Bounded by evicting the least recently seen
    // client, which is safe by construction: the only question ever asked of this
    // map is whether some other client called RECENTLY, so the entry evicted is
    // always one that stopped calling long before any live one.
    TMap<FString, FClientRecord> GClients;
    constexpr int32 GMaxTrackedClients = 32;

    // The caller of the RPC currently on the game thread. Overwritten by the next
    // dispatch rather than cleared: only a handler body reads it, and while a
    // handler runs it is that handler's own caller.
    FString GCallerId;

    // Caller must hold GMutex.
    void RecordActivity(const FString& ClientId, const FString& Method, double AtSeconds)
    {
        FClientRecord& Record = GClients.FindOrAdd(ClientId);
        Record.LastMethod = Method;
        Record.LastSeenSeconds = AtSeconds;

        while (GClients.Num() > GMaxTrackedClients)
        {
            FString OldestId;
            double OldestSeconds = TNumericLimits<double>::Max();
            for (const TPair<FString, FClientRecord>& Pair : GClients)
            {
                if (Pair.Value.LastSeenSeconds < OldestSeconds)
                {
                    OldestSeconds = Pair.Value.LastSeenSeconds;
                    OldestId = Pair.Key;
                }
            }
            // The key is copied out before the erase: removing by a reference that
            // aliases the map's own key would destroy the argument mid-call.
            GClients.Remove(OldestId);
        }
    }
}

void NoteRequestClient(const FString& RequestId, const FString& ClientId)
{
    if (RequestId.IsEmpty() || ClientId.IsEmpty())
    {
        return;
    }

    FScopeLock Lock(&GMutex);
    if (!GPendingClients.Contains(RequestId))
    {
        GPendingOrder.Add(RequestId);
    }
    GPendingClients.Add(RequestId, ClientId);

    while (GPendingOrder.Num() > GMaxPendingClients)
    {
        GPendingClients.Remove(GPendingOrder[0]);
        GPendingOrder.RemoveAt(0);
    }
}

void NoteDispatch(const FString& RequestId, const FString& Method)
{
    const double Now = FPlatformTime::Seconds();

    FScopeLock Lock(&GMutex);

    FString ClientId;
    if (const FString* Parked = GPendingClients.Find(RequestId))
    {
        ClientId = *Parked;
        GPendingClients.Remove(RequestId);
        GPendingOrder.RemoveSingle(RequestId);
    }

    GCallerId = ClientId;
    RecordActivity(ClientId, Method, Now);
}

FString GetCallerId()
{
    FScopeLock Lock(&GMutex);
    return GCallerId;
}

bool GetMostRecentOtherClient(const FString& CallerId, FActivity& Out)
{
    const double Now = FPlatformTime::Seconds();

    FScopeLock Lock(&GMutex);

    const FString* BestId = nullptr;
    const FClientRecord* Best = nullptr;
    for (const TPair<FString, FClientRecord>& Pair : GClients)
    {
        if (Pair.Key == CallerId)
        {
            continue;
        }
        if (!Best || Pair.Value.LastSeenSeconds > Best->LastSeenSeconds)
        {
            BestId = &Pair.Key;
            Best = &Pair.Value;
        }
    }

    if (!Best)
    {
        return false;
    }

    Out.ClientId = *BestId;
    Out.Method = Best->LastMethod;
    // Clamped at zero so a clock that moved backwards reports "just now" rather
    // than a negative age that would read as "outside the window".
    Out.SecondsAgo = FMath::Max(0.0, Now - Best->LastSeenSeconds);
    return true;
}

void ResetForTests()
{
    FScopeLock Lock(&GMutex);
    GPendingClients.Empty();
    GPendingOrder.Empty();
    GClients.Empty();
    GCallerId.Reset();
}

void NoteDispatchForTests(const FString& ClientId, const FString& Method, double AtSeconds)
{
    FScopeLock Lock(&GMutex);
    RecordActivity(ClientId, Method, AtSeconds);
}
}
