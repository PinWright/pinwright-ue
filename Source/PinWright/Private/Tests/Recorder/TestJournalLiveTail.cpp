// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FJournalLiveTail — the in-memory live journal tail in the
// PinWrightRecorder module. Exercises the class directly (no session / file IO):
// monotonic cursor, delta-since-cursor exactness + cursor advance, seq/timestamp
// ordering, variable latest-wins coalescing, ring overflow eviction + stale-cursor
// loss signalling, and empty-since-latest. The PinWright module links the recorder
// module (PINWRIGHTRECORDER_API), so the test reaches the public type directly.
#include "Misc/AutomationTest.h"

#include "JournalLiveTail.h"
#include "JournalTypes.h"

namespace
{
    // FName helpers keep the test bodies terse.
    FORCEINLINE FName N(const ANSICHAR* In) { return FName(In); }

    // An empty event prop list (most tests don't assert on props).
    TArray<TPair<FName, FRecordedValue>> NoProps() { return {}; }
}

// ============================================================================
// Monotonic cursor: each push (event or variable) advances the single shared
// cursor by exactly one, regardless of stream.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveTailMonotonicCursorTest,
    "PinWright.recorder.livetail.MonotonicCursor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveTailMonotonicCursorTest::RunTest(const FString& Parameters)
{
    FJournalLiveTail Tail;

    TestEqual(TEXT("cursor starts at 0"), (int64)Tail.GetCursor(), (int64)0);

    Tail.PushEvent(1.0, EJournalDomain::None, N("actor"), N("spawn"), EJournalSeverity::Info, NoProps());
    TestEqual(TEXT("first event -> cursor 1"), (int64)Tail.GetCursor(), (int64)1);

    Tail.PushVariable(2.0, EJournalDomain::Physics, N("actor"), N("vel"), FRecordedValue::From(10.0));
    TestEqual(TEXT("variable advances shared cursor -> 2"), (int64)Tail.GetCursor(), (int64)2);

    Tail.PushEvent(3.0, EJournalDomain::None, N("actor"), N("crash"), EJournalSeverity::Warning, NoProps());
    TestEqual(TEXT("second event -> cursor 3"), (int64)Tail.GetCursor(), (int64)3);

    // Updating an existing series still consumes a fresh cursor id (monotonic).
    Tail.PushVariable(4.0, EJournalDomain::Physics, N("actor"), N("vel"), FRecordedValue::From(20.0));
    TestEqual(TEXT("series update advances cursor -> 4"), (int64)Tail.GetCursor(), (int64)4);

    return true;
}

// ============================================================================
// Delta-since-cursor: a query returns exactly the entries after the cursor and
// reports the new high-water; re-querying at the returned cursor yields nothing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveTailDeltaSinceCursorTest,
    "PinWright.recorder.livetail.DeltaSinceCursor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveTailDeltaSinceCursorTest::RunTest(const FString& Parameters)
{
    FJournalLiveTail Tail;

    Tail.PushEvent(1.0, EJournalDomain::None, N("actor"), N("spawn"), EJournalSeverity::Info, NoProps());
    Tail.PushVariable(2.0, EJournalDomain::Physics, N("actor"), N("vel"), FRecordedValue::From(1.0));

    // Full read from the beginning returns both entries and the current high-water.
    FLiveTailDelta First = Tail.QuerySince(0);
    TestEqual(TEXT("from 0: one event"), First.Events.Num(), 1);
    TestEqual(TEXT("from 0: one variable"), First.Variables.Num(), 1);
    TestEqual(TEXT("from 0: cursor == high-water 2"), (int64)First.Cursor, (int64)2);
    TestFalse(TEXT("from 0: no loss"), First.bLostData);

    // Nothing new since the returned cursor.
    FLiveTailDelta Empty = Tail.QuerySince(First.Cursor);
    TestEqual(TEXT("empty since latest: no events"), Empty.Events.Num(), 0);
    TestEqual(TEXT("empty since latest: no variables"), Empty.Variables.Num(), 0);
    TestEqual(TEXT("empty since latest: cursor unchanged"), (int64)Empty.Cursor, (int64)2);

    // Push more, then a delta from the prior cursor returns ONLY the new entries.
    Tail.PushVariable(3.0, EJournalDomain::Physics, N("actor"), N("alt"), FRecordedValue::From(5.0));
    Tail.PushEvent(4.0, EJournalDomain::None, N("actor"), N("checkpoint"), EJournalSeverity::Info, NoProps());

    FLiveTailDelta Next = Tail.QuerySince(First.Cursor);
    TestEqual(TEXT("delta: exactly one new event"), Next.Events.Num(), 1);
    TestEqual(TEXT("delta: exactly one new variable"), Next.Variables.Num(), 1);
    TestEqual(TEXT("delta: new event seq is 4"), (int64)Next.Events[0].Seq, (int64)4);
    TestEqual(TEXT("delta: new variable seq is 3"), (int64)Next.Variables[0].Seq, (int64)3);
    TestTrue(TEXT("delta: every new entry seq > sinceCursor (event)"),
        Next.Events[0].Seq > First.Cursor);
    TestTrue(TEXT("delta: every new entry seq > sinceCursor (variable)"),
        Next.Variables[0].Seq > First.Cursor);
    TestEqual(TEXT("delta: cursor advanced to 4"), (int64)Next.Cursor, (int64)4);

    return true;
}

// ============================================================================
// Ordering: events come back ascending by seq (FIFO); variables ascending by
// last-change seq; an updated series carries its latest value and latest seq.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveTailOrderingTest,
    "PinWright.recorder.livetail.Ordering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveTailOrderingTest::RunTest(const FString& Parameters)
{
    FJournalLiveTail Tail;

    // Interleave three series and three events.
    Tail.PushVariable(1.0, EJournalDomain::Physics, N("actor"), N("a"), FRecordedValue::From(1.0)); // seq 1
    Tail.PushEvent(1.1, EJournalDomain::None, N("actor"), N("e1"), EJournalSeverity::Info, NoProps()); // seq 2
    Tail.PushVariable(1.2, EJournalDomain::Physics, N("actor"), N("b"), FRecordedValue::From(2.0)); // seq 3
    Tail.PushEvent(1.3, EJournalDomain::None, N("actor"), N("e2"), EJournalSeverity::Info, NoProps()); // seq 4
    Tail.PushVariable(1.4, EJournalDomain::Physics, N("actor"), N("c"), FRecordedValue::From(3.0)); // seq 5
    Tail.PushEvent(1.5, EJournalDomain::None, N("actor"), N("e3"), EJournalSeverity::Info, NoProps()); // seq 6

    // Update series "a" — it should jump to the newest seq with its new value, not duplicate.
    Tail.PushVariable(1.6, EJournalDomain::Physics, N("actor"), N("a"), FRecordedValue::From(9.0)); // seq 7

    FLiveTailDelta Delta = Tail.QuerySince(0);

    TestEqual(TEXT("three events retained"), Delta.Events.Num(), 3);
    for (int32 i = 1; i < Delta.Events.Num(); ++i)
    {
        TestTrue(TEXT("events ascending by seq"), Delta.Events[i].Seq > Delta.Events[i - 1].Seq);
        TestTrue(TEXT("events ascending by timestamp"), Delta.Events[i].Ts > Delta.Events[i - 1].Ts);
    }

    // Latest-wins: still three distinct series, not four.
    TestEqual(TEXT("three distinct variable series (a coalesced)"), Delta.Variables.Num(), 3);
    for (int32 i = 1; i < Delta.Variables.Num(); ++i)
    {
        TestTrue(TEXT("variables ascending by seq"), Delta.Variables[i].Seq > Delta.Variables[i - 1].Seq);
    }

    // The last variable in seq order is "a" (re-stamped to seq 7) carrying its updated value.
    const FLiveTailVariable& Newest = Delta.Variables.Last();
    TestEqual(TEXT("newest variable is the updated series 'a'"), Newest.Tag, N("a"));
    TestEqual(TEXT("updated series carries latest seq 7"), (int64)Newest.Seq, (int64)7);
    TestEqual(TEXT("updated series carries latest value 9.0"), Newest.Value.F0, 9.0);

    return true;
}

// ============================================================================
// Overflow: the event ring drops the oldest and a query with a stale cursor
// signals loss; querying past the evicted watermark does not.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveTailEventOverflowTest,
    "PinWright.recorder.livetail.EventOverflowSignalsLoss",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveTailEventOverflowTest::RunTest(const FString& Parameters)
{
    // Tiny event ring (cap 3); generous variable cap.
    FJournalLiveTail Tail(/*MaxEvents=*/3, /*MaxVariables=*/64);

    for (int32 i = 1; i <= 5; ++i)
    {
        Tail.PushEvent((double)i, EJournalDomain::None, N("actor"),
            FName(*FString::Printf(TEXT("e%d"), i)), EJournalSeverity::Info, NoProps());
    }

    // Seqs 1..5 assigned; ring holds only the newest 3 (seq 3,4,5); seq 1,2 evicted.
    TestEqual(TEXT("cursor at 5"), (int64)Tail.GetCursor(), (int64)5);
    TestEqual(TEXT("two entries dropped"), Tail.GetDroppedCount(), 2);

    FLiveTailDelta FromZero = Tail.QuerySince(0);
    TestEqual(TEXT("ring retains exactly 3 events"), FromZero.Events.Num(), 3);
    TestEqual(TEXT("oldest retained is seq 3"), (int64)FromZero.Events[0].Seq, (int64)3);
    TestEqual(TEXT("newest retained is seq 5"), (int64)FromZero.Events.Last().Seq, (int64)5);
    TestTrue(TEXT("stale cursor (0) signals loss"), FromZero.bLostData);
    TestEqual(TEXT("delta reports lifetime dropped count"), FromZero.DroppedCount, 2);

    // Cursor at/after the evicted high-water (2) is not loss — those entries were already consumed.
    FLiveTailDelta FromTwo = Tail.QuerySince(2);
    TestFalse(TEXT("cursor at evicted high-water: no loss"), FromTwo.bLostData);
    TestEqual(TEXT("from 2: still returns the 3 retained events"), FromTwo.Events.Num(), 3);

    FLiveTailDelta FromLatest = Tail.QuerySince(5);
    TestFalse(TEXT("cursor at latest: no loss"), FromLatest.bLostData);
    TestEqual(TEXT("cursor at latest: no events"), FromLatest.Events.Num(), 0);

    return true;
}

// ============================================================================
// Variable overflow: evict the least-recently-changed series and signal loss;
// a same-series update is coalescing, NOT a loss.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveTailVariableOverflowTest,
    "PinWright.recorder.livetail.VariableOverflowSignalsLoss",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveTailVariableOverflowTest::RunTest(const FString& Parameters)
{
    // Series cap of 2; generous event cap.
    FJournalLiveTail Tail(/*MaxEvents=*/64, /*MaxVariables=*/2);

    Tail.PushVariable(1.0, EJournalDomain::Physics, N("actor"), N("a"), FRecordedValue::From(1.0)); // seq 1
    Tail.PushVariable(2.0, EJournalDomain::Physics, N("actor"), N("b"), FRecordedValue::From(2.0)); // seq 2

    // Updating an existing series must NOT evict — map stays at 2, no loss.
    Tail.PushVariable(3.0, EJournalDomain::Physics, N("actor"), N("a"), FRecordedValue::From(11.0)); // seq 3
    TestEqual(TEXT("same-series update is not an eviction"), Tail.GetDroppedCount(), 0);
    TestFalse(TEXT("update path does not signal loss"), Tail.QuerySince(0).bLostData);

    // A third DISTINCT series overflows the cap; the least-recently-changed survivor is evicted.
    // After the update above, series 'b' (seq 2) is now the least-recently-changed, so 'b' is evicted.
    Tail.PushVariable(4.0, EJournalDomain::Physics, N("actor"), N("c"), FRecordedValue::From(3.0)); // seq 4
    TestEqual(TEXT("one series evicted on overflow"), Tail.GetDroppedCount(), 1);

    FLiveTailDelta FromZero = Tail.QuerySince(0);
    TestEqual(TEXT("two series retained after eviction"), FromZero.Variables.Num(), 2);
    TestTrue(TEXT("stale cursor signals variable loss"), FromZero.bLostData);

    // Survivors are 'a' (re-stamped seq 3) and 'c' (seq 4); 'b' (seq 2) is gone.
    bool bHasA = false;
    bool bHasB = false;
    bool bHasC = false;
    for (const FLiveTailVariable& V : FromZero.Variables)
    {
        bHasA |= (V.Tag == N("a"));
        bHasB |= (V.Tag == N("b"));
        bHasC |= (V.Tag == N("c"));
    }
    TestTrue(TEXT("series 'a' retained"), bHasA);
    TestFalse(TEXT("least-recently-changed series 'b' evicted"), bHasB);
    TestTrue(TEXT("series 'c' retained"), bHasC);

    // Cursor at the evicted high-water (2) clears the loss flag.
    TestFalse(TEXT("cursor past evicted seq: no loss"), Tail.QuerySince(2).bLostData);

    return true;
}

// ============================================================================
// Empty-since-latest: a fresh-but-active tail returns nothing past its cursor.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveTailEmptySinceLatestTest,
    "PinWright.recorder.livetail.EmptySinceLatest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveTailEmptySinceLatestTest::RunTest(const FString& Parameters)
{
    FJournalLiveTail Tail;

    // No pushes yet: query from 0 is empty and loss-free.
    FLiveTailDelta Initial = Tail.QuerySince(0);
    TestEqual(TEXT("fresh tail: no events"), Initial.Events.Num(), 0);
    TestEqual(TEXT("fresh tail: no variables"), Initial.Variables.Num(), 0);
    TestEqual(TEXT("fresh tail: cursor 0"), (int64)Initial.Cursor, (int64)0);
    TestFalse(TEXT("fresh tail: no loss"), Initial.bLostData);

    Tail.PushEvent(1.0, EJournalDomain::None, N("actor"), N("spawn"), EJournalSeverity::Info, NoProps());
    Tail.PushVariable(2.0, EJournalDomain::Physics, N("actor"), N("vel"), FRecordedValue::From(7.0));

    const uint64 Latest = Tail.GetCursor();
    FLiveTailDelta Drained = Tail.QuerySince(Latest);
    TestEqual(TEXT("since latest: no events"), Drained.Events.Num(), 0);
    TestEqual(TEXT("since latest: no variables"), Drained.Variables.Num(), 0);
    TestEqual(TEXT("since latest: cursor unchanged"), (int64)Drained.Cursor, (int64)Latest);
    TestFalse(TEXT("since latest: no loss"), Drained.bLostData);

    return true;
}
