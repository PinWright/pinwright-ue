// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/SoundCueDumpBuilder.h"
#include "SCIR/SCIRDecompiler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetDumpTestHelpers.h"
#include "Tests/TestUtils.h"


#include "Curves/RichCurve.h"
#include "Engine/Attenuation.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::LoadJsonFile;

    FString MakeUniqueSoundCueTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    USoundCue* NewTransientSoundCue(FString& OutObjectPath, USoundNodeMixer*& OutMixer,
                                     USoundNodeWavePlayer*& OutWavePlayer, USoundWave*& OutSoundWave)
    {
        const FString AssetName = MakeUniqueSoundCueTestAssetName(TEXT("SC_SoundCueDump"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        USoundCue* Cue = NewObject<USoundCue>(
            Package,
            USoundCue::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Cue)
        {
            return nullptr;
        }

        USoundNodeMixer* Mixer = Cue->ConstructSoundNode<USoundNodeMixer>();
        USoundNodeWavePlayer* WavePlayer = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
        USoundWave* SoundWave = NewObject<USoundWave>(GetTransientPackage(), USoundWave::StaticClass(), NAME_None, RF_Transient);
        WavePlayer->SetSoundWave(SoundWave);

        Mixer->ChildNodes.Add(WavePlayer);
        Cue->FirstNode = Mixer;

        Cue->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        OutMixer = Mixer;
        OutWavePlayer = WavePlayer;
        OutSoundWave = SoundWave;
        return Cue;
    }

    TSharedPtr<FJsonObject> FindNodeByPath(const TArray<TSharedPtr<FJsonValue>>* Nodes, const FString& Path)
    {
        if (!Nodes)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Nodes)
        {
            TSharedPtr<FJsonObject> Obj = Value->AsObject();
            if (Obj.IsValid() && Obj->GetStringField(TEXT("path")) == Path)
            {
                return Obj;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueDumpBuilderShapeTest,
    "PinWright.Assets.SoundCue.DumpBuilder.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueDumpBuilderShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundNodeMixer* Mixer = nullptr;
    USoundNodeWavePlayer* WavePlayer = nullptr;
    USoundWave* SoundWave = nullptr;
    USoundCue* Cue = NewTransientSoundCue(ObjectPath, Mixer, WavePlayer, SoundWave);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    TSharedPtr<FJsonObject> CueJson = SoundCueDumpBuilder::BuildSoundCueJson(Cue);
    TestTrue(TEXT("BuildSoundCueJson returns non-null"), CueJson.IsValid());
    if (!CueJson.IsValid())
    {
        Cue->RemoveFromRoot();
        return false;
    }

    TestEqual(TEXT("assetKind"), CueJson->GetStringField(TEXT("assetKind")), FString(TEXT("SoundCue")));
    TestEqual(TEXT("path"), CueJson->GetStringField(TEXT("path")), Cue->GetPathName());
    TestEqual(TEXT("firstNode"), CueJson->GetStringField(TEXT("firstNode")), Mixer->GetPathName());

    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    TestTrue(TEXT("nodes array exists"), CueJson->TryGetArrayField(TEXT("nodes"), Nodes));
    TestTrue(TEXT("nodes array has >= 2 entries"), Nodes && Nodes->Num() >= 2);

    TSharedPtr<FJsonObject> MixerEntry = FindNodeByPath(Nodes, Mixer->GetPathName());
    TSharedPtr<FJsonObject> WaveEntry = FindNodeByPath(Nodes, WavePlayer->GetPathName());
    TestTrue(TEXT("mixer entry present"), MixerEntry.IsValid());
    TestTrue(TEXT("wave player entry present"), WaveEntry.IsValid());

    if (MixerEntry.IsValid())
    {
        TestEqual(TEXT("mixer className"), MixerEntry->GetStringField(TEXT("className")), FString(TEXT("SoundNodeMixer")));
        TestTrue(TEXT("mixer properties object exists"), MixerEntry->HasTypedField<EJson::Object>(TEXT("properties")));
        const TArray<TSharedPtr<FJsonValue>>* MixerEdges = nullptr;
        TestTrue(TEXT("mixer edges exist"), MixerEntry->TryGetArrayField(TEXT("edges"), MixerEdges));
        TestTrue(TEXT("mixer has one edge"), MixerEdges && MixerEdges->Num() == 1);
        if (MixerEdges && MixerEdges->Num() == 1)
        {
            TestEqual(TEXT("mixer edge[0] points at wave player"),
                (*MixerEdges)[0]->AsString(), WavePlayer->GetPathName());
        }
    }

    if (WaveEntry.IsValid())
    {
        TestEqual(TEXT("wave player className"), WaveEntry->GetStringField(TEXT("className")), FString(TEXT("SoundNodeWavePlayer")));
        TestTrue(TEXT("wave player properties object exists"), WaveEntry->HasTypedField<EJson::Object>(TEXT("properties")));
        TestEqual(TEXT("wave player soundWave resolves to fixture wave"),
            WaveEntry->GetStringField(TEXT("soundWave")), SoundWave->GetPathName());
    }

    Cue->RemoveFromRoot();
    return true;
}

// Regression guard for E-describe-sound-cue-omits-cue-level-fields: BuildSoundCueJson (shared by
// describe_sound_cue and the sound_cue.json sidecar) must surface the cue-level settings the
// set_cue_concurrency / set_cue_attenuation setters write — concurrency, attenuation, soundClass,
// volume, pitch — not only the node graph. Reverting the fix drops these fields and fails here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueDumpBuilderCueLevelFieldsTest,
    "PinWright.Assets.SoundCue.DumpBuilder.CueLevelFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueDumpBuilderCueLevelFieldsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundNodeMixer* Mixer = nullptr;
    USoundNodeWavePlayer* WavePlayer = nullptr;
    USoundWave* SoundWave = nullptr;
    USoundCue* Cue = NewTransientSoundCue(ObjectPath, Mixer, WavePlayer, SoundWave);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    // Author the cue-level fields in-code (no external content dependency), exactly what the
    // set_cue_concurrency / set_cue_attenuation setters write onto the cue.
    USoundConcurrency* Concurrency = NewObject<USoundConcurrency>(
        GetTransientPackage(), USoundConcurrency::StaticClass(), NAME_None, RF_Transient);
    USoundAttenuation* Attenuation = NewObject<USoundAttenuation>(
        GetTransientPackage(), USoundAttenuation::StaticClass(), NAME_None, RF_Transient);
    USoundClass* SoundClass = NewObject<USoundClass>(
        GetTransientPackage(), USoundClass::StaticClass(), NAME_None, RF_Transient);
    TestNotNull(TEXT("Concurrency fixture created"), Concurrency);
    TestNotNull(TEXT("Attenuation fixture created"), Attenuation);
    TestNotNull(TEXT("SoundClass fixture created"), SoundClass);
    if (!Concurrency || !Attenuation || !SoundClass)
    {
        Cue->RemoveFromRoot();
        return false;
    }

    Cue->ConcurrencySet.Add(Concurrency);
    Cue->AttenuationSettings = Attenuation;
    Cue->SoundClassObject = SoundClass;
    // 0.375 and 1.625 are exactly representable in binary floating point, so the float->double
    // JSON round-trip is exact and the equality asserts below need no tolerance fuzz.
    Cue->VolumeMultiplier = 0.375f;
    Cue->PitchMultiplier = 1.625f;

    TSharedPtr<FJsonObject> CueJson = SoundCueDumpBuilder::BuildSoundCueJson(Cue);
    TestTrue(TEXT("BuildSoundCueJson returns non-null"), CueJson.IsValid());
    if (!CueJson.IsValid())
    {
        Cue->RemoveFromRoot();
        return false;
    }

    // concurrency: array carrying the set concurrency asset's path.
    const TArray<TSharedPtr<FJsonValue>>* ConcurrencyValues = nullptr;
    TestTrue(TEXT("concurrency array present"),
        CueJson->TryGetArrayField(TEXT("concurrency"), ConcurrencyValues));
    if (ConcurrencyValues)
    {
        TestEqual(TEXT("concurrency has one entry"), ConcurrencyValues->Num(), 1);
        if (ConcurrencyValues->Num() == 1)
        {
            TestEqual(TEXT("concurrency[0] path"),
                (*ConcurrencyValues)[0]->AsString(), Concurrency->GetPathName());
        }
    }

    // attenuation / soundClass: the referenced asset paths.
    TestEqual(TEXT("attenuation path"),
        CueJson->GetStringField(TEXT("attenuation")), Attenuation->GetPathName());
    TestEqual(TEXT("soundClass path"),
        CueJson->GetStringField(TEXT("soundClass")), SoundClass->GetPathName());

    // volume / pitch multipliers.
    double Volume = 0.0;
    TestTrue(TEXT("volume field present"), CueJson->TryGetNumberField(TEXT("volume"), Volume));
    TestEqual(TEXT("volume value"), Volume, 0.375);
    double Pitch = 0.0;
    TestTrue(TEXT("pitch field present"), CueJson->TryGetNumberField(TEXT("pitch"), Pitch));
    TestEqual(TEXT("pitch value"), Pitch, 1.625);

    Cue->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueDumpBuilderConcurrencyOrderingTest,
    "PinWright.Assets.SoundCue.DumpBuilder.ConcurrencyOrdering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueDumpBuilderConcurrencyOrderingTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundNodeMixer* Mixer = nullptr;
    USoundNodeWavePlayer* WavePlayer = nullptr;
    USoundWave* SoundWave = nullptr;
    USoundCue* Cue = NewTransientSoundCue(ObjectPath, Mixer, WavePlayer, SoundWave);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    USoundConcurrency* Zeta = NewObject<USoundConcurrency>(
        GetTransientPackage(), TEXT("ZetaConcurrency"));
    USoundConcurrency* Alpha = NewObject<USoundConcurrency>(
        GetTransientPackage(), TEXT("AlphaConcurrency"));
    Cue->ConcurrencySet.Add(Zeta);
    Cue->ConcurrencySet.Add(Alpha);

    const TSharedPtr<FJsonObject> CueJson = SoundCueDumpBuilder::BuildSoundCueJson(Cue);
    const TArray<TSharedPtr<FJsonValue>>* ConcurrencyValues = nullptr;
    TestTrue(TEXT("concurrency array present"),
        CueJson.IsValid() && CueJson->TryGetArrayField(TEXT("concurrency"), ConcurrencyValues));
    if (ConcurrencyValues)
    {
        TestEqual(TEXT("concurrency has two entries"), ConcurrencyValues->Num(), 2);
        if (ConcurrencyValues->Num() == 2)
        {
            TestEqual(TEXT("concurrency[0] is lexically first"),
                (*ConcurrencyValues)[0]->AsString(), Alpha->GetPathName());
            TestEqual(TEXT("concurrency[1] is lexically second"),
                (*ConcurrencyValues)[1]->AsString(), Zeta->GetPathName());
        }
    }

    Cue->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueAssetDumpWritesSoundCueAspectFileTest,
    "PinWright.Assets.SoundCue.AssetDump.WritesSoundCueAspectFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueAssetDumpWritesSoundCueAspectFileTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundNodeMixer* Mixer = nullptr;
    USoundNodeWavePlayer* WavePlayer = nullptr;
    USoundWave* SoundWave = nullptr;
    USoundCue* Cue = NewTransientSoundCue(ObjectPath, Mixer, WavePlayer, SoundWave);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("SoundCueDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient SoundCue"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Properties));
    TestTrue(TEXT("sound_cue.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::SoundCue));

    const FString CuePath = FindDumpFile(Result.WrittenPaths, DumpFileNames::SoundCue);
    TSharedPtr<FJsonObject> CueJson = LoadJsonFile(CuePath);
    TestTrue(TEXT("sound_cue.json parses"), CueJson.IsValid());
    if (CueJson.IsValid())
    {
        TestEqual(TEXT("sound_cue.json assetKind"),
            CueJson->GetStringField(TEXT("assetKind")), FString(TEXT("SoundCue")));

        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        TestTrue(TEXT("sound_cue.json nodes array exists"), CueJson->TryGetArrayField(TEXT("nodes"), Nodes));
        TestTrue(TEXT("sound_cue.json nodes array has >= 2 entries"), Nodes && Nodes->Num() >= 2);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Cue->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueSCIRDecompilerShapeTest,
    "PinWright.Assets.SoundCue.SCIRDecompiler.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueSCIRDecompilerShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundNodeMixer* Mixer = nullptr;
    USoundNodeWavePlayer* WavePlayer = nullptr;
    USoundWave* SoundWave = nullptr;
    USoundCue* Cue = NewTransientSoundCue(ObjectPath, Mixer, WavePlayer, SoundWave);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    const FSCIRResult Result = SCIRDecompiler::BuildSoundCueIrText(Cue);
    TestTrue(TEXT("BuildSoundCueIrText succeeds"), Result.bSuccess);
    TestTrue(TEXT("SCIR contains sound_cue entry"), Result.Text.Contains(TEXT("sound_cue")));
    TestTrue(TEXT("SCIR contains root mixer"), Result.Text.Contains(TEXT("root mixer")));
    TestTrue(TEXT("SCIR contains child wave_player"), Result.Text.Contains(TEXT("child wave_player")));
    TestTrue(TEXT("SCIR contains wave object path"), Result.Text.Contains(SoundWave->GetPathName()));
    TestFalse(TEXT("SCIR does not emit cue-level SoundGroup"), Result.Text.Contains(TEXT("SoundGroup")));

    Cue->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueSCIRDecompilerAudioMetadataTest,
    "PinWright.Assets.SoundCue.SCIRDecompiler.AudioMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueSCIRDecompilerAudioMetadataTest::RunTest(const FString& Parameters)
{
    const FString AssetName = MakeUniqueSoundCueTestAssetName(TEXT("SC_SoundCueSCIRAudio"));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    USoundCue* Cue = NewObject<USoundCue>(
        Package,
        USoundCue::StaticClass(),
        *AssetName,
        RF_Public | RF_Standalone | RF_Transient);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    USoundNodeBranch* Branch = Cue->ConstructSoundNode<USoundNodeBranch>();
    USoundNodeAttenuation* Attenuation = Cue->ConstructSoundNode<USoundNodeAttenuation>();
    USoundNodeModulator* Modulator = Cue->ConstructSoundNode<USoundNodeModulator>();
    USoundNodeWavePlayer* WavePlayer = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
    USoundWave* SoundWave = NewObject<USoundWave>(GetTransientPackage(), USoundWave::StaticClass(), NAME_None, RF_Transient);

    TestNotNull(TEXT("Branch node created"), Branch);
    TestNotNull(TEXT("Attenuation node created"), Attenuation);
    TestNotNull(TEXT("Modulator node created"), Modulator);
    TestNotNull(TEXT("Wave player node created"), WavePlayer);
    TestNotNull(TEXT("SoundWave created"), SoundWave);
    if (!Branch || !Attenuation || !Modulator || !WavePlayer || !SoundWave)
    {
        return false;
    }

    Branch->BoolParameterName = TEXT("UseDetailedCue");
    Branch->ChildNodes.Add(Attenuation);
    Attenuation->ChildNodes.Add(Modulator);
    Modulator->ChildNodes.Add(WavePlayer);
    WavePlayer->SetSoundWave(SoundWave);

    SoundWave->Duration = 2.5f;
    SoundWave->NumChannels = 2;
    SoundWave->SetSampleRate(44100);

    Attenuation->bOverrideAttenuation = true;
    Attenuation->AttenuationOverrides.bSpatialize = true;
    Attenuation->AttenuationOverrides.SpatializationAlgorithm = ESoundSpatializationAlgorithm::SPATIALIZATION_HRTF;
    Attenuation->AttenuationOverrides.DistanceAlgorithm = EAttenuationDistanceModel::Logarithmic;
    Attenuation->AttenuationOverrides.AttenuationShape = EAttenuationShape::Box;
    Attenuation->AttenuationOverrides.AttenuationShapeExtents = FVector(100.0, 200.0, 300.0);
    Attenuation->AttenuationOverrides.FalloffDistance = 1234.0f;
    Attenuation->AttenuationOverrides.dBAttenuationAtMax = -12.0f;
    Attenuation->AttenuationOverrides.CustomAttenuationCurve.GetRichCurve()->AddKey(0.0f, 1.0f);

    Modulator->PitchMin = 0.8f;
    Modulator->PitchMax = 1.2f;
    Modulator->VolumeMin = 0.25f;
    Modulator->VolumeMax = 0.75f;

    Cue->FirstNode = Branch;
    Cue->AddToRoot();

    const FSCIRResult Result = SCIRDecompiler::BuildSoundCueIrText(Cue);
    TestTrue(TEXT("BuildSoundCueIrText succeeds"), Result.bSuccess);
    TestTrue(TEXT("SCIR contains branch root"), Result.Text.Contains(TEXT("root branch")));
    TestTrue(TEXT("SCIR contains attenuation child"), Result.Text.Contains(TEXT("child attenuation")));
    TestTrue(TEXT("SCIR contains modulator child"), Result.Text.Contains(TEXT("child modulator")));
    TestTrue(TEXT("SCIR contains wave player child"), Result.Text.Contains(TEXT("child wave_player")));
    TestTrue(TEXT("SCIR contains wave duration"), Result.Text.Contains(TEXT("duration: 2.5")));
    TestTrue(TEXT("SCIR contains wave channel count"), Result.Text.Contains(TEXT("numChannels: 2")));
    TestTrue(TEXT("SCIR contains raw wave sample rate"), Result.Text.Contains(TEXT("sampleRate: 44100")));
    TestTrue(TEXT("SCIR contains branch parameter"), Result.Text.Contains(TEXT("BoolParameterName: \"UseDetailedCue\"")));
    TestTrue(TEXT("SCIR contains spatialization flag"), Result.Text.Contains(TEXT("spatialize: true")));
    TestTrue(TEXT("SCIR contains readable distance algorithm"), Result.Text.Contains(TEXT("distanceAlgorithm: \"Logarithmic\"")));
    TestTrue(TEXT("SCIR contains readable attenuation shape"), Result.Text.Contains(TEXT("attenuationShape: \"Box\"")));
    TestTrue(TEXT("SCIR contains attenuation falloff"), Result.Text.Contains(TEXT("falloffDistance: 1234")));
    TestTrue(TEXT("SCIR contains custom attenuation curve presence"), Result.Text.Contains(TEXT("hasCustomAttenuationCurve: true")));
    TestTrue(TEXT("SCIR contains modulator pitch min"), Result.Text.Contains(TEXT("PitchMin: 0.8")));
    TestTrue(TEXT("SCIR contains modulator pitch max"), Result.Text.Contains(TEXT("PitchMax: 1.2")));
    TestTrue(TEXT("SCIR contains modulator volume min"), Result.Text.Contains(TEXT("VolumeMin: 0.25")));
    TestTrue(TEXT("SCIR contains modulator volume max"), Result.Text.Contains(TEXT("VolumeMax: 0.75")));

    Cue->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueAssetDumpWritesSCIRSidecarTest,
    "PinWright.Assets.SoundCue.AssetDump.WritesSCIRSidecar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueAssetDumpWritesSCIRSidecarTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundNodeMixer* Mixer = nullptr;
    USoundNodeWavePlayer* WavePlayer = nullptr;
    USoundWave* SoundWave = nullptr;
    USoundCue* Cue = NewTransientSoundCue(ObjectPath, Mixer, WavePlayer, SoundWave);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("SoundCueSCIRDumpTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient SoundCue"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("sound_cue.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::SoundCue));
    TestTrue(TEXT("scir.txt is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Scir));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Cue->RemoveFromRoot();
    return true;
}
