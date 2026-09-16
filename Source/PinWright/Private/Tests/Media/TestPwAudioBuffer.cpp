// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the AudioGen currency types: FPwAudioBuffer (deinterleaved stereo PCM) and
// FPwSeededRandom (reproducible per-layer substreams).
//
// Every case here is written so it breaks in the direction the type is supposed to prevent:
// a resize that touches one channel, a mix that grows or overruns the target, a rate mismatch
// that pitch-shifts a layer instead of refusing, and a Derive that leaks evaluation order into
// the render. Asserting only the happy path would pass against all four regressions.
//
// The MixInto cases assert its RETURNED frame count as well as the samples, so each clip case
// makes the stronger of the two available claims: not "the buffer did not change" but "the
// function reported that it did not change". A mix that quietly wrote nothing and a mix that
// reported writing frames it did not write both fail here.

#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwSeededRandom.h"

// Named (not anonymous) so unity merges cannot ODR-collide these with a sibling test file's
// helpers, per the shared-helper convention in the plugin CLAUDE.md.
namespace PwAudioBufferTestHelpers
{
    // NumFrames frames holding a constant value in each channel.
    FPwAudioBuffer MakeConstant(int32 NumFrames, float LeftValue, float RightValue, int32 SampleRate)
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = SampleRate;
        Buffer.SetNumFrames(NumFrames);
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            Buffer.Left[Frame] = LeftValue;
            Buffer.Right[Frame] = RightValue;
        }
        return Buffer;
    }

    // Sum of |sample| over both channels: one number that moves if ANY sample was written,
    // used to assert that a refused or out-of-range mix touched nothing at all.
    float AbsSum(const FPwAudioBuffer& Buffer)
    {
        float Total = 0.0f;
        for (const float Sample : Buffer.Left)
        {
            Total += FMath::Abs(Sample);
        }
        for (const float Sample : Buffer.Right)
        {
            Total += FMath::Abs(Sample);
        }
        return Total;
    }

    // Draws Count samples from Rng in [-1, 1].
    TArray<float> Draw(FPwSeededRandom& Rng, int32 Count)
    {
        TArray<float> Samples;
        Samples.Reserve(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Samples.Add(Rng.FloatInRange(-1.0f, 1.0f));
        }
        return Samples;
    }
}

// =========================================================================
// A. SetNumFrames is the only resize that owns the Left.Num() == Right.Num()
//    invariant. Both channels are asserted directly rather than through
//    NumFrames() (which reads Left), so a resize that forgets one side fails.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioBufferSetNumFramesTest,
    "PinWright.audio.buffer.SetNumFramesKeepsChannelsEqual",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioBufferSetNumFramesTest::RunTest(const FString& Parameters)
{
    FPwAudioBuffer Buffer;
    TestEqual(TEXT("A default buffer holds no frames"), Buffer.NumFrames(), 0);
    TestEqual(TEXT("A default buffer runs at 48 kHz"), Buffer.SampleRate, 48000);
    TestTrue(TEXT("A default buffer is valid"), Buffer.IsValid());

    Buffer.SetNumFrames(100);
    TestEqual(TEXT("Grow: Left holds 100 frames"), Buffer.Left.Num(), 100);
    TestEqual(TEXT("Grow: Right holds 100 frames"), Buffer.Right.Num(), 100);
    TestEqual(TEXT("Grow: Left is zero-filled"), Buffer.Left[0], 0.0f);
    TestEqual(TEXT("Grow: Right is zero-filled"), Buffer.Right[99], 0.0f);

    Buffer.Left[10] = 0.25f;
    Buffer.Right[10] = -0.25f;
    Buffer.SetNumFrames(200);
    TestEqual(TEXT("Regrow: Left holds 200 frames"), Buffer.Left.Num(), 200);
    TestEqual(TEXT("Regrow: Right holds 200 frames"), Buffer.Right.Num(), 200);
    TestEqual(TEXT("Regrow preserves the existing Left samples"), Buffer.Left[10], 0.25f);
    TestEqual(TEXT("Regrow preserves the existing Right samples"), Buffer.Right[10], -0.25f);
    TestEqual(TEXT("Regrow zero-fills the new Left frames"), Buffer.Left[150], 0.0f);
    TestEqual(TEXT("Regrow zero-fills the new Right frames"), Buffer.Right[150], 0.0f);

    Buffer.SetNumFrames(50);
    TestEqual(TEXT("Shrink: Left holds 50 frames"), Buffer.Left.Num(), 50);
    TestEqual(TEXT("Shrink: Right holds 50 frames"), Buffer.Right.Num(), 50);

    // bZeroed=false changes the fill, never the invariant.
    Buffer.SetNumFrames(75, false);
    TestEqual(TEXT("Uninitialized grow: Left holds 75 frames"), Buffer.Left.Num(), 75);
    TestEqual(TEXT("Uninitialized grow: Right holds 75 frames"), Buffer.Right.Num(), 75);

    // A negative count clamps to empty rather than tripping TArray's own bounds check.
    Buffer.SetNumFrames(-5);
    TestEqual(TEXT("A negative count empties Left"), Buffer.Left.Num(), 0);
    TestEqual(TEXT("A negative count empties Right"), Buffer.Right.Num(), 0);

    TestTrue(TEXT("IsValid holds after every resize"), Buffer.IsValid());

    // Failure direction for IsValid: it must reject a hand-filled buffer whose channels
    // disagree, otherwise the assertion above would also pass for a constant true.
    FPwAudioBuffer Lopsided;
    Lopsided.Left.SetNumZeroed(8);
    TestFalse(TEXT("Mismatched channel lengths are not valid"), Lopsided.IsValid());

    FPwAudioBuffer NoRate;
    NoRate.SampleRate = 0;
    TestFalse(TEXT("A zero sample rate is not valid"), NoRate.IsValid());

    return true;
}

// =========================================================================
// B. DurationSeconds for known frame counts, and the unset-rate case, which
//    must report zero rather than dividing by zero and handing the caller an
//    infinity that reads downstream as an extremely long sound.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioBufferDurationSecondsTest,
    "PinWright.audio.buffer.DurationSeconds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioBufferDurationSecondsTest::RunTest(const FString& Parameters)
{
    FPwAudioBuffer Buffer;
    TestEqual(TEXT("An empty buffer is zero seconds long"), Buffer.DurationSeconds(), 0.0f);

    Buffer.SetNumFrames(48000);
    TestEqual(TEXT("48000 frames at 48 kHz is one second"), Buffer.DurationSeconds(), 1.0f);

    Buffer.SetNumFrames(24000);
    TestEqual(TEXT("24000 frames at 48 kHz is half a second"), Buffer.DurationSeconds(), 0.5f);

    FPwAudioBuffer AtCdRate;
    AtCdRate.SampleRate = 44100;
    AtCdRate.SetNumFrames(22050);
    TestEqual(TEXT("22050 frames at 44.1 kHz is half a second"), AtCdRate.DurationSeconds(), 0.5f);

    FPwAudioBuffer NoRate;
    NoRate.SetNumFrames(1000);
    NoRate.SampleRate = 0;
    TestEqual(TEXT("A zero sample rate reports zero duration"), NoRate.DurationSeconds(), 0.0f);
    TestTrue(TEXT("A zero sample rate never reports a non-finite duration"),
        FMath::IsFinite(NoRate.DurationSeconds()));

    return true;
}

// =========================================================================
// C. MixInto clips the write window to the target at BOTH ends and never
//    grows it. The out-of-range cases are asserted against a whole-buffer
//    sum, so a single stray sample anywhere fails.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioBufferMixIntoClampsTest,
    "PinWright.audio.buffer.MixIntoClamps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioBufferMixIntoClampsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioBufferTestHelpers;

    const FPwAudioBuffer Source = MakeConstant(4, 1.0f, 2.0f, 48000);

    // Tail clip: two of the four source frames fall inside the target, and the gain is
    // applied to what survives.
    {
        FPwAudioBuffer Target = MakeConstant(10, 0.0f, 0.0f, 48000);
        TestEqual(TEXT("Tail clip reports the 2 frames that fit, not the 4 requested"),
            Source.MixInto(Target, 0.5f, 8), 2);
        TestEqual(TEXT("Tail clip does not grow Left"), Target.Left.Num(), 10);
        TestEqual(TEXT("Tail clip does not grow Right"), Target.Right.Num(), 10);
        TestEqual(TEXT("Tail clip writes Left frame 8 at gain"), Target.Left[8], 0.5f);
        TestEqual(TEXT("Tail clip writes Left frame 9 at gain"), Target.Left[9], 0.5f);
        TestEqual(TEXT("Tail clip writes Right frame 8 at gain"), Target.Right[8], 1.0f);
        TestEqual(TEXT("Tail clip writes Right frame 9 at gain"), Target.Right[9], 1.0f);
        TestEqual(TEXT("Tail clip leaves the frame before the start untouched"), Target.Left[7], 0.0f);
    }

    // Head clip: a negative start DROPS the leading source frames rather than shifting the
    // layer forward, so source frames 2 and 3 land at target frames 0 and 1.
    {
        FPwAudioBuffer Target = MakeConstant(10, 0.0f, 0.0f, 48000);
        TestEqual(TEXT("Head clip reports the 2 surviving frames"),
            Source.MixInto(Target, 1.0f, -2), 2);
        TestEqual(TEXT("Head clip does not grow Left"), Target.Left.Num(), 10);
        TestEqual(TEXT("Head clip does not grow Right"), Target.Right.Num(), 10);
        TestEqual(TEXT("Head clip writes target frame 0"), Target.Left[0], 1.0f);
        TestEqual(TEXT("Head clip writes target frame 1"), Target.Left[1], 1.0f);
        TestEqual(TEXT("Head clip stops after the two surviving frames"), Target.Left[2], 0.0f);
    }

    // Fully out of range in both directions: nothing written, nothing grown. -4 is exactly
    // -SrcFrames and 10 exactly DstFrames, so both boundary values are covered.
    {
        FPwAudioBuffer Target = MakeConstant(10, 0.0f, 0.0f, 48000);
        const int32 OutOfRangeStarts[] = { 10, 11, 1000, MAX_int32, -4, -5, -1000, MIN_int32 };
        for (const int32 Start : OutOfRangeStarts)
        {
            TestEqual(FString::Printf(TEXT("Start %d reports 0 frames mixed"), Start),
                Source.MixInto(Target, 1.0f, Start), 0);
        }
        TestEqual(TEXT("Out-of-range starts write nothing at all"), AbsSum(Target), 0.0f);
        TestEqual(TEXT("Out-of-range starts never grow Left"), Target.Left.Num(), 10);
        TestEqual(TEXT("Out-of-range starts never grow Right"), Target.Right.Num(), 10);
    }

    // Boundary control: 9 is the last in-range start and -3 the first, so the rejects above
    // are a clip at the real edges and not a blanket refusal one frame too eager.
    {
        FPwAudioBuffer Target = MakeConstant(10, 0.0f, 0.0f, 48000);
        TestEqual(TEXT("Start 9 reports 1 frame mixed"), Source.MixInto(Target, 1.0f, 9), 1);
        TestEqual(TEXT("Start 9 writes the last target frame"), Target.Left[9], 1.0f);
        TestEqual(TEXT("Start 9 writes nothing before it"), Target.Left[8], 0.0f);

        FPwAudioBuffer Earliest = MakeConstant(10, 0.0f, 0.0f, 48000);
        TestEqual(TEXT("Start -3 reports 1 frame mixed"), Source.MixInto(Earliest, 1.0f, -3), 1);
        TestEqual(TEXT("Start -3 writes the first target frame"), Earliest.Left[0], 1.0f);
        TestEqual(TEXT("Start -3 writes nothing after it"), Earliest.Left[1], 0.0f);
    }

    // A short target truncates a longer layer instead of being extended by it.
    {
        FPwAudioBuffer Short = MakeConstant(2, 0.0f, 0.0f, 48000);
        TestEqual(TEXT("A truncated layer reports the target's length, not its own"),
            Source.MixInto(Short, 1.0f, 0), 2);
        TestEqual(TEXT("A layer longer than the target does not extend it"), Short.Left.Num(), 2);
        TestEqual(TEXT("A layer longer than the target still fills it"), AbsSum(Short), 6.0f);
    }

    // MixInto adds; it does not replace. A second pass at the same offset accumulates.
    {
        FPwAudioBuffer Target = MakeConstant(10, 0.0f, 0.0f, 48000);
        TestEqual(TEXT("A fully in-range mix reports all 4 frames"),
            Source.MixInto(Target, 1.0f, 0), 4);
        TestEqual(TEXT("The second mix reports all 4 frames too"),
            Source.MixInto(Target, 1.0f, 0), 4);
        TestEqual(TEXT("A second mix at the same offset accumulates"), Target.Left[0], 2.0f);
    }

    // An empty source contributes nothing and cannot resize the target.
    {
        FPwAudioBuffer Empty;
        FPwAudioBuffer Target = MakeConstant(10, 0.0f, 0.0f, 48000);
        TestEqual(TEXT("An empty source reports 0 frames mixed"),
            Empty.MixInto(Target, 1.0f, 0), 0);
        TestEqual(TEXT("An empty source writes nothing"), AbsSum(Target), 0.0f);
        TestEqual(TEXT("An empty source does not resize the target"), Target.Left.Num(), 10);
    }

    return true;
}

// =========================================================================
// D. A sample-rate mismatch is refused, not resampled and not asserted. The
//    refusal is only useful if it is discoverable, so the expected warning is
//    declared: if the log line is ever dropped, this test fails on the
//    unmatched expectation.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioBufferMixIntoSampleRateMismatchTest,
    "PinWright.audio.buffer.MixIntoRejectsSampleRateMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioBufferMixIntoSampleRateMismatchTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioBufferTestHelpers;

    AddExpectedMessage(TEXT("MixInto skipped"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 1);

    FPwAudioBuffer Source = MakeConstant(4, 1.0f, 1.0f, 44100);
    FPwAudioBuffer Target = MakeConstant(4, 0.0f, 0.0f, 48000);

    TestEqual(TEXT("A rate mismatch reports 0 frames mixed"),
        Source.MixInto(Target, 1.0f, 0), 0);
    TestEqual(TEXT("A 44.1 kHz source writes nothing into a 48 kHz target"), AbsSum(Target), 0.0f);
    TestEqual(TEXT("The refused mix does not resize the target"), Target.Left.Num(), 4);

    // Control: the identical call lands once the rates agree, so the assertions above are
    // measuring the rate gate rather than a mix that is broken outright.
    Source.SampleRate = 48000;
    TestEqual(TEXT("The matched-rate mix reports all 4 frames"),
        Source.MixInto(Target, 1.0f, 0), 4);
    TestEqual(TEXT("The same mix lands once the rates match"), AbsSum(Target), 8.0f);

    return true;
}

// =========================================================================
// E. Derive is order-independent and non-consuming - the property the whole
//    reproducible-render promise rests on.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSeededRandomDeriveOrderIndependenceTest,
    "PinWright.audio.random.DeriveIsOrderIndependent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSeededRandomDeriveOrderIndependenceTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioBufferTestHelpers;

    constexpr int32 Seed = 1234;
    constexpr int32 TargetStream = 3;

    // Stream 3 taken first, from an untouched parent.
    TArray<float> Direct;
    {
        FPwSeededRandom Parent(Seed);
        FPwSeededRandom Child = Parent.Derive(TargetStream);
        Direct = Draw(Child, 8);
    }

    // Stream 3 taken after streams 0-2 were derived AND drawn from. If Derive advanced the
    // parent, or seeded from the parent's live position, this sequence diverges.
    TArray<float> AfterSiblings;
    {
        FPwSeededRandom Parent(Seed);
        for (int32 Index = 0; Index < TargetStream; ++Index)
        {
            FPwSeededRandom Sibling = Parent.Derive(Index);
            Draw(Sibling, 5);
        }
        FPwSeededRandom Child = Parent.Derive(TargetStream);
        AfterSiblings = Draw(Child, 8);
    }

    TestEqual(TEXT("Both orderings produce the same sample count"),
        AfterSiblings.Num(), Direct.Num());
    for (int32 Index = 0; Index < Direct.Num() && Index < AfterSiblings.Num(); ++Index)
    {
        TestEqual(FString::Printf(TEXT("Stream %d sample %d is order-independent"),
            TargetStream, Index), AfterSiblings[Index], Direct[Index]);
    }

    // Deriving must not consume from the parent: the parent's own next draw is unchanged.
    {
        FPwSeededRandom Untouched(Seed);
        const float Expected = Untouched.FloatInRange(-1.0f, 1.0f);

        FPwSeededRandom Derived(Seed);
        Derived.Derive(7);
        Derived.Derive(11);
        TestEqual(TEXT("Deriving does not advance the parent stream"),
            Derived.FloatInRange(-1.0f, 1.0f), Expected);
    }

    // Failure direction: distinct indices must seed DISTINCT streams. A Derive that ignored
    // StreamIndex, or returned a copy of the parent, satisfies both assertions above.
    {
        FPwSeededRandom Parent(Seed);
        const FPwSeededRandom StreamThree = Parent.Derive(3);
        const FPwSeededRandom StreamFour = Parent.Derive(4);
        TestTrue(TEXT("Different stream indices seed different streams"),
            StreamThree.Stream.GetInitialSeed() != StreamFour.Stream.GetInitialSeed());
        TestTrue(TEXT("A substream is not a copy of its parent"),
            StreamThree.Stream.GetInitialSeed() != Parent.Stream.GetInitialSeed());
    }

    return true;
}

// =========================================================================
// F. The same seed reproduces the same sequence, different seeds do not, and
//    both range helpers respect their bounds.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSeededRandomReproducibilityTest,
    "PinWright.audio.random.ReproducibleFromSameSeed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSeededRandomReproducibilityTest::RunTest(const FString& Parameters)
{
    constexpr int32 Seed = 20260816;

    {
        FPwSeededRandom First(Seed);
        FPwSeededRandom Second(Seed);
        for (int32 Index = 0; Index < 16; ++Index)
        {
            const float A = First.FloatInRange(-1.0f, 1.0f);
            const float B = Second.FloatInRange(-1.0f, 1.0f);
            TestEqual(FString::Printf(TEXT("Float draw %d reproduces"), Index), B, A);

            const int32 IntA = First.IntInRange(0, 1000);
            const int32 IntB = Second.IntInRange(0, 1000);
            TestEqual(FString::Printf(TEXT("Int draw %d reproduces"), Index), IntB, IntA);
        }
    }

    // Failure direction: a different seed must give a different sequence, otherwise the
    // reproducibility assertions above would also hold for a constant generator.
    {
        FPwSeededRandom Base(Seed);
        FPwSeededRandom Other(Seed + 1);
        bool bAnyDifference = false;
        for (int32 Index = 0; Index < 16 && !bAnyDifference; ++Index)
        {
            bAnyDifference = (Base.FloatInRange(-1.0f, 1.0f) != Other.FloatInRange(-1.0f, 1.0f));
        }
        TestTrue(TEXT("A different seed produces a different sequence"), bAnyDifference);
    }

    // Bounds, including the degenerate single-value integer range.
    {
        FPwSeededRandom Rng(7);
        for (int32 Index = 0; Index < 64; ++Index)
        {
            const float Value = Rng.FloatInRange(2.0f, 3.0f);
            TestTrue(FString::Printf(TEXT("Float draw %d stays inside [2, 3]"), Index),
                Value >= 2.0f && Value <= 3.0f);

            const int32 Bounded = Rng.IntInRange(-5, 5);
            TestTrue(FString::Printf(TEXT("Int draw %d stays inside [-5, 5]"), Index),
                Bounded >= -5 && Bounded <= 5);

            TestEqual(TEXT("A single-value integer range returns that value"),
                Rng.IntInRange(4, 4), 4);
        }
    }

    return true;
}
