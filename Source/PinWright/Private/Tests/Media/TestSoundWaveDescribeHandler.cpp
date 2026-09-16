// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

#include "Sound/SoundWave.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDescribeSoundWaveReturnsDumpShapeTest,
    "PinWright.audio.authoring.describe_sound_wave.ReturnsDumpShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDescribeSoundWaveReturnsDumpShapeTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SW_DescribeWave_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    USoundWave* Wave = NewObject<USoundWave>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);

    TestNotNull(TEXT("Temporary SoundWave created"), Wave);
    if (!Wave)
    {
        return false;
    }

    Wave->AddToRoot();
    ON_SCOPE_EXIT
    {
        Wave->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    // Seed a non-default compression value so the describe readback must echo it back —
    // regression guard for the describe/sidecar omission this test previously tolerated.
    // Write it through the real set_sound_wave_properties handler (the canonical writer that
    // owns the private-since-5.6 CompressionQuality reflection) rather than re-rolling that
    // reflection here, then verify describe returns the exact value.
    // Every seeded value is NON-DEFAULT and distinct from every other, which is what makes the
    // readback assertions below discriminating: a describe that hardcoded engine defaults, or one
    // that transposed volume and pitch, produces a well-typed response that fails here.
    constexpr int32 ExpectedCompressionQuality = 73;
    constexpr double ExpectedVolume = 0.6;   // default 1.0
    constexpr double ExpectedPitch = 1.4;    // default 1.0, and != ExpectedVolume so a swap shows
    const FString ExpectedSoundGroup = TEXT("SOUNDGROUP_Voice"); // default SOUNDGROUP_Default
    {
        TSharedPtr<FJsonObject> SeedPayload = MakeShared<FJsonObject>();
        SeedPayload->SetStringField(TEXT("assetPath"), Wave->GetPathName());
        SeedPayload->SetNumberField(TEXT("compressionQuality"), ExpectedCompressionQuality);
        SeedPayload->SetBoolField(TEXT("bLooping"), true);           // default false
        SeedPayload->SetNumberField(TEXT("volume"), ExpectedVolume);
        SeedPayload->SetNumberField(TEXT("pitch"), ExpectedPitch);
        SeedPayload->SetStringField(TEXT("soundGroup"), ExpectedSoundGroup);
        SeedPayload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture SeedCapture;
        const bool bSeedFound = InvokeHandlerWithCapture(
            TEXT("audio.authoring.set_sound_wave_properties"), SeedPayload, SeedCapture);
        TestTrue(TEXT("Seed handler found"), bSeedFound);
        TestTrue(FString::Printf(TEXT("Seed handler succeeded (errorCode='%s')"),
            *SeedCapture.ErrorCode), SeedCapture.bSuccess);
        if (!bSeedFound || !SeedCapture.bSuccess)
        {
            return false;
        }
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Wave->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.describe_sound_wave"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("duration is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("duration")));
    TestTrue(TEXT("numChannels is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("numChannels")));
    TestTrue(TEXT("sampleRate is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("sampleRate")));
    TestTrue(TEXT("bLooping is a bool"),
        Capture.Result->HasTypedField<EJson::Boolean>(TEXT("bLooping")));
    TestTrue(TEXT("soundGroup is a string"),
        Capture.Result->HasTypedField<EJson::String>(TEXT("soundGroup")));
    TestTrue(TEXT("volume is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("volume")));
    TestTrue(TEXT("pitch is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("pitch")));

    // Presence-and-type alone cannot fail on a describe that reports the wrong thing: this
    // fixture is a bare NewObject<USoundWave>, so a BuildSoundWaveJson emitting hardcoded
    // defaults (or reading the wrong UProperty) satisfies every HasTypedField above. Assert the
    // VALUES seeded through the canonical writer instead, so describe is checked against what
    // was actually written rather than against its own choice of shape.
    bool DescribedLooping = false;
    Capture.Result->TryGetBoolField(TEXT("bLooping"), DescribedLooping);
    TestTrue(TEXT("bLooping round-trips the written value (true)"), DescribedLooping);

    FString DescribedSoundGroup;
    Capture.Result->TryGetStringField(TEXT("soundGroup"), DescribedSoundGroup);
    TestEqual(TEXT("soundGroup round-trips the written value"),
        DescribedSoundGroup, ExpectedSoundGroup);

    double DescribedVolume = 0.0;
    Capture.Result->TryGetNumberField(TEXT("volume"), DescribedVolume);
    TestTrue(FString::Printf(TEXT("volume round-trips the written value (got %.4f, want %.4f)"),
        DescribedVolume, ExpectedVolume),
        FMath::IsNearlyEqual(DescribedVolume, ExpectedVolume, 1.0e-4));

    double DescribedPitch = 0.0;
    Capture.Result->TryGetNumberField(TEXT("pitch"), DescribedPitch);
    TestTrue(FString::Printf(TEXT("pitch round-trips the written value (got %.4f, want %.4f)"),
        DescribedPitch, ExpectedPitch),
        FMath::IsNearlyEqual(DescribedPitch, ExpectedPitch, 1.0e-4));

    // The wiki promises describe_sound_wave carries "compression settings"; the sibling writer
    // set_sound_wave_properties writes compressionQuality. Assert describe both emits the field
    // and round-trips the value written above — this fails if BuildSoundWaveJson drops it.
    TestTrue(TEXT("compressionQuality is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("compressionQuality")));
    TestEqual(TEXT("compressionQuality round-trips the written value"),
        Capture.Result->GetIntegerField(TEXT("compressionQuality")), ExpectedCompressionQuality);

    return true;
}
