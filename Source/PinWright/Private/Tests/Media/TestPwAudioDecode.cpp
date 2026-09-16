// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for PwDecodeSoundWave (AudioGen/PwAudioDecode.h).
//
// Every case here asserts the FAILURE direction as well as the value
// (rpc-design.md §12): each rejection test checks the specific error code AND that
// the output buffer was left empty, so a regression that reports success - or that
// half-fills the buffer before bailing - breaks the test rather than sliding past it.
//
// Fixture pattern follows TestSoundWaveAuthoringHandler.cpp's MakeTransientWave:
// a transient USoundWave kept alive with AddToRoot and released in ON_SCOPE_EXIT.
// The waves here live in the transient package rather than /Game/, because none of
// them is ever saved, so no CleanupTestAsset is needed. Payloads are built with the
// engine's own SerializeWaveFile (Audio.h:999) instead of a binary fixture on disk.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "Handlers/ErrorCodes.h"

#include "Audio.h"
#include "Memory/SharedBuffer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// Named namespace, not anonymous: TestSoundWaveAuthoringHandler.cpp already has an
// anonymous-namespace MakeTransientWave in this same directory, and Unity would
// merge the two into one TU. See CLAUDE.md > Building.
namespace PwAudioDecodeTest
{
    // Bare transient wave with no imported payload at all.
    USoundWave* MakeBareWave()
    {
        USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), NAME_None, RF_Transient);
        if (Wave)
        {
            Wave->AddToRoot();
        }
        return Wave;
    }

    // Transient wave carrying an imported 16-bit PCM payload.
    //
    // USoundWave::NumChannels must be set: GetImportedSoundWaveData's mono/stereo
    // path asserts check(NumChannels > 0) against the UPROPERTY (SoundWave.cpp:1961)
    // and uses it to trim a trailing partial frame, while the sample rate and the
    // reported channel count both come out of the wave header instead.
    USoundWave* MakeWaveWithPcm(const TArray<int16>& InterleavedPcm, int32 NumChannels, int32 SampleRate)
    {
        USoundWave* Wave = MakeBareWave();
        if (!Wave)
        {
            return nullptr;
        }

        Wave->NumChannels = NumChannels;

        TArray<uint8> WavBytes;
        SerializeWaveFile(WavBytes,
            reinterpret_cast<const uint8*>(InterleavedPcm.GetData()),
            InterleavedPcm.Num() * static_cast<int32>(sizeof(int16)),
            NumChannels, SampleRate);

        Wave->RawData.UpdatePayload(FSharedBuffer::Clone(WavBytes.GetData(), WavBytes.Num()));
        return Wave;
    }

    // A buffer with content in it, used to prove a failing decode empties the
    // caller's output rather than leaving stale data behind.
    FPwAudioBuffer MakeDirtyBuffer()
    {
        FPwAudioBuffer Dirty;
        Dirty.SampleRate = 8000;
        Dirty.SetNumFrames(16, /*bZeroed=*/true);
        return Dirty;
    }

    constexpr float SampleTolerance = 1.e-6f;
}

// =========================================================================
// A. Stereo round-trip: a synthesized 16-bit PCM payload comes back with the
//    expected frame count, sample rate, and per-channel sample values.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioDecodeStereoRoundTripTest,
    "PinWright.audio.decode.StereoRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioDecodeStereoRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioDecodeTest;

    // Rails included deliberately: -32768 must land on exactly -1.0.
    const TArray<int16> Left  = { 0,  1000, -1000, 32767, -32768 };
    const TArray<int16> Right = { -1, 2000, -2000, 16384, -16384 };

    TArray<int16> Interleaved;
    Interleaved.Reserve(Left.Num() * 2);
    for (int32 Frame = 0; Frame < Left.Num(); ++Frame)
    {
        Interleaved.Add(Left[Frame]);
        Interleaved.Add(Right[Frame]);
    }

    USoundWave* Wave = MakeWaveWithPcm(Interleaved, /*NumChannels=*/2, /*SampleRate=*/44100);
    TestNotNull(TEXT("Transient stereo SoundWave created"), Wave);
    if (!Wave) return false;

    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    FPwAudioBuffer Out;
    FString ErrorCode;
    FString Error;
    const bool bDecoded = PwDecodeSoundWaveWithCode(Wave, Out, ErrorCode, Error);

    TestTrue(FString::Printf(TEXT("Decode succeeded (code='%s', error='%s')"), *ErrorCode, *Error),
        bDecoded);
    TestTrue(TEXT("No error code on success"), ErrorCode.IsEmpty());
    TestTrue(TEXT("No error message on success"), Error.IsEmpty());
    if (!bDecoded) return false;

    TestEqual(TEXT("Frame count matches the source PCM"), Out.NumFrames(), Left.Num());
    TestEqual(TEXT("Sample rate comes from the wave header"), Out.SampleRate, 44100);
    TestEqual(TEXT("Left channel is sized to the frame count"), Out.Left.Num(), Left.Num());
    TestEqual(TEXT("Right channel is sized to the frame count"), Out.Right.Num(), Left.Num());
    if (Out.Left.Num() != Left.Num() || Out.Right.Num() != Right.Num()) return false;

    constexpr float Scale = 1.0f / 32768.0f;
    for (int32 Frame = 0; Frame < Left.Num(); ++Frame)
    {
        TestEqual(FString::Printf(TEXT("Left[%d] deinterleaved"), Frame),
            Out.Left[Frame], static_cast<float>(Left[Frame]) * Scale, SampleTolerance);
        TestEqual(FString::Printf(TEXT("Right[%d] deinterleaved"), Frame),
            Out.Right[Frame], static_cast<float>(Right[Frame]) * Scale, SampleTolerance);
    }

    // The channels must not have been swapped or copied from each other.
    // (TestNotEqual has no float overload, so this is an explicit comparison.)
    TestTrue(TEXT("Channels are distinct (frame 0 differs)"), Out.Left[0] != Out.Right[0]);
    TestEqual(TEXT("Negative full scale maps to exactly -1.0"), Out.Left[4], -1.0f, SampleTolerance);

    return true;
}

// =========================================================================
// B. Mono duplicates into both channels — Right is always allocated, and it
//    carries the same samples as Left rather than silence.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioDecodeMonoDuplicatesTest,
    "PinWright.audio.decode.MonoDuplicatesIntoBothChannels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioDecodeMonoDuplicatesTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioDecodeTest;

    const TArray<int16> Mono = { 0, 8192, -8192, 32767 };

    USoundWave* Wave = MakeWaveWithPcm(Mono, /*NumChannels=*/1, /*SampleRate=*/22050);
    TestNotNull(TEXT("Transient mono SoundWave created"), Wave);
    if (!Wave) return false;

    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    FPwAudioBuffer Out;
    FString ErrorCode;
    FString Error;
    const bool bDecoded = PwDecodeSoundWaveWithCode(Wave, Out, ErrorCode, Error);

    TestTrue(FString::Printf(TEXT("Decode succeeded (code='%s', error='%s')"), *ErrorCode, *Error),
        bDecoded);
    if (!bDecoded) return false;

    TestEqual(TEXT("Frame count matches the source PCM"), Out.NumFrames(), Mono.Num());
    TestEqual(TEXT("Sample rate comes from the wave header"), Out.SampleRate, 22050);
    TestEqual(TEXT("Right channel is allocated for mono input"), Out.Right.Num(), Mono.Num());
    if (Out.Left.Num() != Mono.Num() || Out.Right.Num() != Mono.Num()) return false;

    constexpr float Scale = 1.0f / 32768.0f;
    for (int32 Frame = 0; Frame < Mono.Num(); ++Frame)
    {
        const float Expected = static_cast<float>(Mono[Frame]) * Scale;
        TestEqual(FString::Printf(TEXT("Left[%d] converted"), Frame),
            Out.Left[Frame], Expected, SampleTolerance);
        TestEqual(FString::Printf(TEXT("Right[%d] duplicates Left[%d]"), Frame, Frame),
            Out.Right[Frame], Expected, SampleTolerance);
    }

    // Counterfactual: a Right channel left zeroed would pass the loop above only if
    // every source sample were zero, so assert at least one non-zero sample landed.
    TestTrue(TEXT("Right channel is not silent"), Out.Right[1] != 0.0f);

    return true;
}

// =========================================================================
// C. bProcedural is refused with its own code, and the caller's buffer is
//    emptied. Guard copied from AudioAnalyzerNRT.cpp:139-144.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioDecodeRejectsProceduralTest,
    "PinWright.audio.decode.RejectsProcedural",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioDecodeRejectsProceduralTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioDecodeTest;

    // A real payload is attached first, so the rejection can only come from the
    // bProcedural flag and not from an absent payload.
    const TArray<int16> Mono = { 100, 200, 300, 400 };
    USoundWave* Wave = MakeWaveWithPcm(Mono, /*NumChannels=*/1, /*SampleRate=*/48000);
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;

    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    Wave->bProcedural = 1;

    // Pre-populated so a handler that forgets to clear on failure is caught.
    FPwAudioBuffer Out = MakeDirtyBuffer();
    FString ErrorCode;
    FString Error;
    const bool bDecoded = PwDecodeSoundWaveWithCode(Wave, Out, ErrorCode, Error);

    TestFalse(TEXT("Procedural wave is refused"), bDecoded);
    TestEqual(TEXT("ErrorCode is AUDIO_PROCEDURAL_UNSUPPORTED"),
        ErrorCode, FString(ErrorCodes::ERR_AUDIO_PROCEDURAL_UNSUPPORTED));
    TestFalse(TEXT("Error message is populated"), Error.IsEmpty());
    TestEqual(TEXT("Output buffer emptied: no frames"), Out.NumFrames(), 0);
    TestEqual(TEXT("Output buffer emptied: Left"), Out.Left.Num(), 0);
    TestEqual(TEXT("Output buffer emptied: Right"), Out.Right.Num(), 0);

    return true;
}

// =========================================================================
// D. A non-empty ChannelSizes (the >2-channel deinterleaved-surround layout)
//    is refused with a DIFFERENT code from the procedural case, so a caller
//    never has to parse the message (rpc-design.md §7). Guard copied from
//    AudioAnalyzerNRT.cpp:147-152 / SoundWave.cpp:3006-3010.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioDecodeRejectsMultichannelTest,
    "PinWright.audio.decode.RejectsMultichannel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioDecodeRejectsMultichannelTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioDecodeTest;

    USoundWave* Wave = MakeBareWave();
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;

    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    // The engine's own tell for the surround layout: RawData then holds N
    // concatenated mono RIFF files rather than one interleaved stream.
    Wave->NumChannels = 6;
    Wave->ChannelSizes.Init(0, 6);

    FPwAudioBuffer Out;
    FString ErrorCode;
    FString Error;
    const bool bDecoded = PwDecodeSoundWaveWithCode(Wave, Out, ErrorCode, Error);

    TestFalse(TEXT("Multi-channel wave is refused"), bDecoded);
    TestEqual(TEXT("ErrorCode is AUDIO_MULTICHANNEL_UNSUPPORTED"),
        ErrorCode, FString(ErrorCodes::ERR_AUDIO_MULTICHANNEL_UNSUPPORTED));
    TestNotEqual(TEXT("Multi-channel code differs from the procedural code"),
        ErrorCode, FString(ErrorCodes::ERR_AUDIO_PROCEDURAL_UNSUPPORTED));
    TestEqual(TEXT("Output buffer left empty"), Out.NumFrames(), 0);

    return true;
}

// =========================================================================
// E. A null wave fails rather than returning a silent empty buffer, and the
//    caller's pre-existing buffer contents are discarded.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioDecodeRejectsNullWaveTest,
    "PinWright.audio.decode.RejectsNullWave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioDecodeRejectsNullWaveTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioDecodeTest;

    FPwAudioBuffer Out = MakeDirtyBuffer();
    TestEqual(TEXT("Precondition: buffer starts with content"), Out.NumFrames(), 16);

    FString ErrorCode;
    FString Error;
    const bool bDecoded = PwDecodeSoundWaveWithCode(nullptr, Out, ErrorCode, Error);

    TestFalse(TEXT("Null wave is refused"), bDecoded);
    TestEqual(TEXT("ErrorCode is INVALID_PARAMS"),
        ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestNotEqual(TEXT("A caller bug is not reported as a decode failure"),
        ErrorCode, FString(ErrorCodes::ERR_DECODE_FAILED));
    TestEqual(TEXT("Output buffer emptied: no frames"), Out.NumFrames(), 0);
    TestEqual(TEXT("Output buffer emptied: Left"), Out.Left.Num(), 0);
    TestEqual(TEXT("Output buffer emptied: Right"), Out.Right.Num(), 0);

    return true;
}

// =========================================================================
// F. A wave with no imported payload fails; it must not decode to a silent
//    zero-frame buffer reported as a success.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioDecodeRejectsWaveWithoutPayloadTest,
    "PinWright.audio.decode.RejectsWaveWithoutPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioDecodeRejectsWaveWithoutPayloadTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioDecodeTest;

    // GetImportedSoundWaveData warns on its way out (SoundWave.cpp:1991); expected,
    // and declared so the case still passes on a host that elevates warnings.
    AddExpectedMessage(TEXT("Failed to get imported raw data for sound wave"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

    USoundWave* Wave = MakeBareWave();
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;

    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    Wave->NumChannels = 1;

    FPwAudioBuffer Out;
    FString ErrorCode;
    FString Error;
    const bool bDecoded = PwDecodeSoundWaveWithCode(Wave, Out, ErrorCode, Error);

    TestFalse(TEXT("Payload-less wave is refused"), bDecoded);
    TestEqual(TEXT("ErrorCode is DECODE_FAILED"),
        ErrorCode, FString(ErrorCodes::ERR_DECODE_FAILED));
    TestFalse(TEXT("Error message is populated"), Error.IsEmpty());
    TestEqual(TEXT("Output buffer left empty"), Out.NumFrames(), 0);

    return true;
}

// =========================================================================
// G. The message-only overload agrees with the code-carrying one: same
//    verdict, same message. Guards against the two forms drifting apart.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAudioDecodeOverloadsAgreeTest,
    "PinWright.audio.decode.OverloadsAgree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAudioDecodeOverloadsAgreeTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioDecodeTest;

    const TArray<int16> Mono = { 1, 2, 3, 4 };
    USoundWave* Wave = MakeWaveWithPcm(Mono, /*NumChannels=*/1, /*SampleRate=*/48000);
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;

    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    Wave->bProcedural = 1;

    FPwAudioBuffer WithCodeOut;
    FString ErrorCode;
    FString WithCodeError;
    const bool bWithCode = PwDecodeSoundWaveWithCode(Wave, WithCodeOut, ErrorCode, WithCodeError);

    FPwAudioBuffer PlainOut;
    FString PlainError;
    const bool bPlain = PwDecodeSoundWave(Wave, PlainOut, PlainError);

    TestFalse(TEXT("Code-carrying form refuses"), bWithCode);
    TestFalse(TEXT("Message-only form refuses"), bPlain);
    TestEqual(TEXT("Both forms produce the same message"), PlainError, WithCodeError);
    TestEqual(TEXT("Message-only form also empties the buffer"), PlainOut.NumFrames(), 0);

    return true;
}
