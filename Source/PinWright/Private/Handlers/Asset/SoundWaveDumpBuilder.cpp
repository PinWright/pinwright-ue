// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/SoundWaveDumpBuilder.h"

#include "Sound/SoundWave.h"
#include "Sound/SoundGroups.h"
#include "UObject/UnrealType.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

int32 SoundWaveDumpBuilder::GetRawSampleRate(const USoundWave* Wave)
{
    if (!Wave)
    {
        return 0;
    }

    static const FIntProperty* SampleRateProp = FindFProperty<FIntProperty>(USoundWave::StaticClass(), TEXT("SampleRate"));
    return SampleRateProp ? SampleRateProp->GetPropertyValue_InContainer(Wave) : 0;
}

TSharedPtr<FJsonObject> SoundWaveDumpBuilder::BuildSoundWaveJson(const USoundWave* Wave)
{
    if (!Wave)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetNumberField(TEXT("duration"), Wave->Duration);
    Root->SetNumberField(TEXT("numChannels"), Wave->NumChannels);

    // Read the raw `SampleRate` UPROPERTY via reflection rather than calling
    // GetSampleRateForCurrentPlatform() — that accessor returns a value that varies with
    // per-platform sample-rate overrides and would cause this dump to drift across
    // platforms / project settings, defeating diff-baseline stability.
    Root->SetNumberField(TEXT("sampleRate"), GetRawSampleRate(Wave));

    Root->SetBoolField(TEXT("bLooping"), Wave->bLooping != 0);

    if (const UEnum* SoundGroupEnum = StaticEnum<ESoundGroup>())
    {
        Root->SetStringField(TEXT("soundGroup"),
            SoundGroupEnum->GetNameStringByValue(static_cast<int64>(Wave->SoundGroup.GetValue())));
    }

    Root->SetNumberField(TEXT("volume"), Wave->Volume);
    Root->SetNumberField(TEXT("pitch"), Wave->Pitch);

    // The compression setting `set_sound_wave_properties` writes (int32 0..100). Read via the
    // getter — UE 5.6 made USoundWave::CompressionQuality private — to honor the wiki's promise
    // that describe_sound_wave / the sound_wave.json sidecar carry "compression settings".
    Root->SetNumberField(TEXT("compressionQuality"), Wave->GetCompressionQuality());

    return Root;
}

namespace
{
    UClass* GetSoundWaveSidecarClass()
    {
        return USoundWave::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildSoundWaveSidecar(UObject* Asset)
    {
        return SoundWaveDumpBuilder::BuildSoundWaveJson(Cast<USoundWave>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("sound_wave"), DumpFileNames::SoundWave,
    &GetSoundWaveSidecarClass, &BuildSoundWaveSidecar,
    nullptr, nullptr, nullptr, 100);
