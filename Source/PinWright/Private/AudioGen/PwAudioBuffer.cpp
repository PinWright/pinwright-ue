// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioBuffer.h"

#include "DSP/FloatArrayMath.h"
#include "PinWrightSubsystem.h"

bool FPwAudioBuffer::IsValid() const
{
    return Left.Num() == Right.Num() && SampleRate > 0;
}

void FPwAudioBuffer::SetNumFrames(int32 InNumFrames, bool bZeroed)
{
    const int32 Clamped = FMath::Max(InNumFrames, 0);
    if (bZeroed)
    {
        Left.SetNumZeroed(Clamped);
        Right.SetNumZeroed(Clamped);
    }
    else
    {
        Left.SetNumUninitialized(Clamped);
        Right.SetNumUninitialized(Clamped);
    }
}

float FPwAudioBuffer::DurationSeconds() const
{
    return (SampleRate > 0)
        ? static_cast<float>(NumFrames()) / static_cast<float>(SampleRate)
        : 0.0f;
}

int32 FPwAudioBuffer::MixInto(FPwAudioBuffer& Target, float GainLinear, int32 StartFrame) const
{
    if (Target.SampleRate != SampleRate)
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("FPwAudioBuffer::MixInto skipped: source is %d Hz but target is %d Hz. ")
            TEXT("Resample the source to the target rate before mixing."),
            SampleRate, Target.SampleRate);
        return 0;
    }

    const int32 SrcFrames = NumFrames();
    const int32 DstFrames = Target.NumFrames();
    if (SrcFrames <= 0 || DstFrames <= 0)
    {
        return 0;
    }

    // Whole-layer rejects before any arithmetic on StartFrame: a layer that ends before the
    // target begins, or begins past its end, contributes nothing. This is also what keeps the
    // offsets below from overflowing on an extreme StartFrame.
    if (StartFrame >= DstFrames || StartFrame <= -SrcFrames)
    {
        return 0;
    }

    const int32 DstStart = FMath::Max(StartFrame, 0);
    const int32 SrcStart = DstStart - StartFrame;
    const int32 Count = FMath::Min(SrcFrames - SrcStart, DstFrames - DstStart);

    // Re-clamp per channel against both arrays' real lengths, and report what that channel
    // actually mixed. Left/Right are public, so a caller that filled only one side cannot make
    // this run off the end of the other.
    auto MixChannel = [GainLinear, SrcStart, DstStart, Count]
        (const TArray<float>& Src, TArray<float>& Dst) -> int32
    {
        const int32 Num = FMath::Min3(Count, Src.Num() - SrcStart, Dst.Num() - DstStart);
        if (Num <= 0)
        {
            return 0;
        }
        Audio::ArrayMixIn(
            MakeArrayView(Src.GetData() + SrcStart, Num),
            MakeArrayView(Dst.GetData() + DstStart, Num),
            GainLinear);
        return Num;
    };

    // Both channels mix Count frames whenever the invariant holds. If a caller broke it, the
    // larger of the two is the honest answer to "how far into the target did this write" -
    // under-reporting a write that happened would hide work rather than reveal it.
    const int32 MixedLeft = MixChannel(Left, Target.Left);
    const int32 MixedRight = MixChannel(Right, Target.Right);
    return FMath::Max(MixedLeft, MixedRight);
}
