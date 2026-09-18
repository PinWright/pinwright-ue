// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "PixelFormat.h"
#include "Sound/SoundWave.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "PhysicsEngine/BodySetup.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "UObject/Package.h"


#include "AssetDumpFixtureHelpers.h"

namespace
{
    using AssetDumpFixtureHelpers::LoadJsonFile;
    using AssetDumpFixtureHelpers::WrittenPathsContains;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpStaticMeshSummaryTest,
    "PinWright.asset.dump.StaticMeshSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual: if StaticMeshDumpBuilder::BuildStaticMeshJson is reverted to return null,
// static_mesh.json is absent from WrittenPaths and the materials assertion fails because the file doesn't exist.
bool FAssetDumpStaticMeshSummaryTest::RunTest(const FString& Parameters)
{
    const FString Suffix      = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpStaticMesh") / Suffix;

    const FString FixturePath = TEXT("/Engine/EditorMeshes/EditorCube.EditorCube");
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FixturePath);
    if (!TestNotNull(TEXT("EditorCube static mesh fixture loads"), Mesh))
    {
        return true;
    }

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(FixturePath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains static_mesh.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::StaticMesh));

    const FString StaticMeshJsonPath = Result.DumpDir / DumpFileNames::StaticMesh;
    TestTrue(TEXT("static_mesh.json exists on disk"),
        IFileManager::Get().FileExists(*StaticMeshJsonPath));

    TSharedPtr<FJsonObject> Json = LoadJsonFile(StaticMeshJsonPath);
    if (!TestTrue(TEXT("static_mesh.json parsed"), Json.IsValid()))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* MaterialsArr = nullptr;
    TestTrue(TEXT("materials field present"),
        Json->TryGetArrayField(TEXT("materials"), MaterialsArr));
    if (MaterialsArr)
    {
        TestEqual(TEXT("materials length matches GetStaticMaterials()"),
            MaterialsArr->Num(), Mesh->GetStaticMaterials().Num());
    }

    const UBodySetup* BodySetup = Mesh->GetBodySetup();
    if (TestNotNull(TEXT("EditorCube body setup fixture exists"), BodySetup))
    {
        FString CollisionTraceFlag;
        TestTrue(TEXT("collisionTraceFlag field present"),
            Json->TryGetStringField(TEXT("collisionTraceFlag"), CollisionTraceFlag));

        const TSharedPtr<FJsonObject>* Collision = nullptr;
        TestTrue(TEXT("collision field present"), Json->TryGetObjectField(TEXT("collision"), Collision));

        const TSharedPtr<FJsonObject>* Elements = nullptr;
        if (Collision)
        {
            TestTrue(TEXT("collision.elements field present"),
                (*Collision)->TryGetObjectField(TEXT("elements"), Elements));
        }

        if (Elements)
        {
            double SphereCount = 0.0;
            double BoxCount = 0.0;
            double SphylCount = 0.0;
            double ConvexCount = 0.0;
            double TaperedCapsuleCount = 0.0;
            TestTrue(TEXT("collision.elements.sphere present"),
                (*Elements)->TryGetNumberField(TEXT("sphere"), SphereCount));
            TestTrue(TEXT("collision.elements.box present"),
                (*Elements)->TryGetNumberField(TEXT("box"), BoxCount));
            TestTrue(TEXT("collision.elements.sphyl present"),
                (*Elements)->TryGetNumberField(TEXT("sphyl"), SphylCount));
            TestTrue(TEXT("collision.elements.convex present"),
                (*Elements)->TryGetNumberField(TEXT("convex"), ConvexCount));
            TestTrue(TEXT("collision.elements.taperedCapsule present"),
                (*Elements)->TryGetNumberField(TEXT("taperedCapsule"), TaperedCapsuleCount));
            TestEqual(TEXT("collision.elements.sphere matches body setup"),
                static_cast<int32>(SphereCount), BodySetup->AggGeom.SphereElems.Num());
            TestEqual(TEXT("collision.elements.box matches body setup"),
                static_cast<int32>(BoxCount), BodySetup->AggGeom.BoxElems.Num());
            TestEqual(TEXT("collision.elements.sphyl matches body setup"),
                static_cast<int32>(SphylCount), BodySetup->AggGeom.SphylElems.Num());
            TestEqual(TEXT("collision.elements.convex matches body setup"),
                static_cast<int32>(ConvexCount), BodySetup->AggGeom.ConvexElems.Num());
            TestEqual(TEXT("collision.elements.taperedCapsule matches body setup"),
                static_cast<int32>(TaperedCapsuleCount), BodySetup->AggGeom.TaperedCapsuleElems.Num());
        }
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpTexture2DSummaryTest,
    "PinWright.asset.dump.Texture2DSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual: if the BuildTexture2DJson emission line in AssetDumpHandler.cpp is reverted,
// texture_2d.json is also written and the assertion WrittenPathsContains(..., 'texture_2d.json') == false fails.
bool FAssetDumpTexture2DSummaryTest::RunTest(const FString& Parameters)
{
    const FString Suffix      = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpTexture2D") / Suffix;

    const FString FixturePath = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
    UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *FixturePath);
    if (!TestNotNull(TEXT("DefaultTexture fixture loads"), Texture))
    {
        return true;
    }

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(FixturePath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains texture.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::Texture));
    // Hardcoded: the DumpFileNames::Texture2D constant no longer exists by design; this guards
    // against accidental reintroduction.
    TestFalse(TEXT("texture_2d.json must not be emitted"),
        WrittenPathsContains(Result.WrittenPaths, TEXT("texture_2d.json")));

    const FString TextureJsonPath = Result.DumpDir / DumpFileNames::Texture;
    TSharedPtr<FJsonObject> Json = LoadJsonFile(TextureJsonPath);
    if (!TestTrue(TEXT("texture.json parsed"), Json.IsValid()))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    FString Kind;
    TestTrue(TEXT("kind field present"), Json->TryGetStringField(TEXT("kind"), Kind));
    TestEqual(TEXT("kind equals Texture2D"), Kind, FString(TEXT("Texture2D")));

    const TSharedPtr<FJsonObject>* SizePtr = nullptr;
    if (TestTrue(TEXT("size field present"), Json->TryGetObjectField(TEXT("size"), SizePtr)))
    {
        double X = 0.0;
        double Y = 0.0;
        TestTrue(TEXT("size.x present"), (*SizePtr)->TryGetNumberField(TEXT("x"), X));
        TestTrue(TEXT("size.y present"), (*SizePtr)->TryGetNumberField(TEXT("y"), Y));
        TestEqual(TEXT("size.x matches GetSizeX()"), static_cast<int32>(X), Texture->GetSizeX());
        TestEqual(TEXT("size.y matches GetSizeY()"), static_cast<int32>(Y), Texture->GetSizeY());
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpGenericTextureSummaryCoversNonTexture2DTest,
    "PinWright.asset.dump.GenericTextureSummary.CoversNonTexture2D",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual: if the handler still only checks Cast<UTexture2D>, each non-Texture2D fixture writes only meta.json/properties.json, WrittenPathsContains(..., DumpFileNames::Texture) is false, and the JSON parse path fails.
bool FAssetDumpGenericTextureSummaryCoversNonTexture2DTest::RunTest(const FString& Parameters)
{
    const FString Suffix      = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpGenericTexture") / Suffix;

    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/AssetDumpTextureCube_%s"), *Suffix);
    // RF_Standalone survives the periodic suite GC; detach so a later /Engine/Transient
    // asset-registry rescan cannot see the fixture.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    const FString AssetName = FString::Printf(TEXT("AssetDumpTextureCube_%s"), *Suffix);
    UTextureRenderTargetCube* RenderTarget = NewObject<UTextureRenderTargetCube>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("TextureRenderTargetCube created"), RenderTarget))
    {
        return true;
    }
    RenderTarget->Init(64, PF_FloatRGBA);

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains texture.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::Texture));

    const FString TextureJsonPath = Result.DumpDir / DumpFileNames::Texture;
    TSharedPtr<FJsonObject> Json = LoadJsonFile(TextureJsonPath);
    if (!TestTrue(TEXT("texture.json parsed"), Json.IsValid()))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    FString Kind;
    TestTrue(TEXT("kind present"), Json->TryGetStringField(TEXT("kind"), Kind));
    TestEqual(TEXT("kind equals class name"), Kind, RenderTarget->GetClass()->GetName());

    const TSharedPtr<FJsonObject>* SizePtr = nullptr;
    if (TestTrue(TEXT("size field present"), Json->TryGetObjectField(TEXT("size"), SizePtr)))
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        TestTrue(TEXT("size.x present"), (*SizePtr)->TryGetNumberField(TEXT("x"), X));
        TestTrue(TEXT("size.y present"), (*SizePtr)->TryGetNumberField(TEXT("y"), Y));
        TestTrue(TEXT("size.z present"), (*SizePtr)->TryGetNumberField(TEXT("z"), Z));
        TestEqual(TEXT("size.x matches GetSurfaceWidth()"), static_cast<int32>(X), static_cast<int32>(RenderTarget->GetSurfaceWidth()));
        TestEqual(TEXT("size.y matches GetSurfaceHeight()"), static_cast<int32>(Y), static_cast<int32>(RenderTarget->GetSurfaceHeight()));
        TestEqual(TEXT("size.z matches GetSurfaceDepth()"), static_cast<int32>(Z), static_cast<int32>(RenderTarget->GetSurfaceDepth()));
    }

    double ArraySize = 0.0;
    TestTrue(TEXT("arraySize present"), Json->TryGetNumberField(TEXT("arraySize"), ArraySize));
    TestEqual(TEXT("arraySize matches GetSurfaceArraySize()"),
        static_cast<int32>(ArraySize), static_cast<int32>(RenderTarget->GetSurfaceArraySize()));

    FString PixelFormat;
    TestTrue(TEXT("pixelFormat present"), Json->TryGetStringField(TEXT("pixelFormat"), PixelFormat));
    TestEqual(TEXT("pixelFormat matches GetFormat()"),
        PixelFormat, FString(GetPixelFormatString(RenderTarget->GetFormat())));

    const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
    if (TestTrue(TEXT("source field present"), Json->TryGetObjectField(TEXT("source"), SourcePtr)))
    {
        FString SourceKind;
        FString SourceFormat;
        TestTrue(TEXT("source.sourceKind present"), (*SourcePtr)->TryGetStringField(TEXT("sourceKind"), SourceKind));
        TestEqual(TEXT("source.sourceKind equals renderTarget"), SourceKind, FString(TEXT("renderTarget")));
        TestTrue(TEXT("source.format present"), (*SourcePtr)->TryGetStringField(TEXT("format"), SourceFormat));
        TestTrue(TEXT("source.format is TSF_*"), SourceFormat.StartsWith(TEXT("TSF_")));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSoundWaveSummaryTest,
    "PinWright.asset.dump.SoundWaveSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual: if SampleRate is read from the platform-specific accessor instead of the member,
// the test fails on platforms with sample-rate overrides because the dumped value drifts from the asset's authored value.
bool FAssetDumpSoundWaveSummaryTest::RunTest(const FString& Parameters)
{
    const FString Suffix      = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpSoundWave") / Suffix;

    // Build a transient SoundWave so the test does not depend on engine-content audio fixtures
    // that may not be present in headless builds.
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/AssetDumpSoundWave_%s"), *Suffix);
    // RF_Standalone survives the periodic suite GC; detach so a later /Engine/Transient
    // asset-registry rescan cannot see the fixture.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }
    const FString AssetName = FString::Printf(TEXT("AssetDumpSoundWave_%s"), *Suffix);
    USoundWave* Wave = NewObject<USoundWave>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("SoundWave created"), Wave))
    {
        return true;
    }

    Wave->Duration = 1.5f;
    Wave->NumChannels = 2;
    Wave->Volume = 0.75f;
    Wave->Pitch = 1.25f;
    Wave->bLooping = 1;
    // Set raw SampleRate UPROPERTY so the test catches a regression where the dump
    // switches to GetSampleRateForCurrentPlatform() — that accessor returns 0 on a
    // transient wave with no platform overrides, while raw SampleRate is 48000.
    Wave->SetSampleRate(48000);

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains sound_wave.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::SoundWave));

    const FString WaveJsonPath = Result.DumpDir / DumpFileNames::SoundWave;
    TSharedPtr<FJsonObject> Json = LoadJsonFile(WaveJsonPath);
    if (!TestTrue(TEXT("sound_wave.json parsed"), Json.IsValid()))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    double NumChannels = 0.0;
    TestTrue(TEXT("numChannels present"), Json->TryGetNumberField(TEXT("numChannels"), NumChannels));
    TestEqual(TEXT("numChannels == 2"), static_cast<int32>(NumChannels), 2);

    bool bLooping = false;
    TestTrue(TEXT("bLooping present"), Json->TryGetBoolField(TEXT("bLooping"), bLooping));
    TestTrue(TEXT("bLooping == true"), bLooping);

    double SampleRate = 0.0;
    TestTrue(TEXT("sampleRate present"), Json->TryGetNumberField(TEXT("sampleRate"), SampleRate));
    TestEqual(TEXT("sampleRate == 48000 (raw UPROPERTY, not platform-resolved)"),
        static_cast<int32>(SampleRate), 48000);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpLevelSequenceSummaryTest,
    "PinWright.asset.dump.LevelSequenceSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpLevelSequenceSummaryTest::RunTest(const FString& Parameters)
{
    const FString Suffix      = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpLevelSequence") / Suffix;

    // Build a transient LevelSequence so the test does not depend on engine-content fixtures.
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/AssetDumpLevelSequence_%s"), *Suffix);
    const FString SubAssetName = FString::Printf(TEXT("AssetDumpSubSequence_%s"), *Suffix);
    // RF_Standalone survives the periodic suite GC, so both sequences need detaching. They share
    // one package and both carry RF_Transient, which makes UObject::IsAsset() false for them: the
    // first DiscardLoadedAssetNoGc therefore sees an assetless package and renames the PACKAGE into
    // /Transient, after which the second object's path no longer resolves. Resolve both handles
    // first, then discard the objects directly - path-based cleanup cannot express this.
    ON_SCOPE_EXIT
    {
        UObject* SubFixture = FindObject<UObject>(nullptr, *(PackagePath + TEXT(".") + SubAssetName));
        UObject* MainFixture = FindObject<UObject>(nullptr, *ToObjectPath(PackagePath));
        PwTestAssetTeardown::DiscardLoadedAssetNoGc(SubFixture);
        PwTestAssetTeardown::DiscardLoadedAssetNoGc(MainFixture);
    };

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }
    const FString AssetName = FString::Printf(TEXT("AssetDumpLevelSequence_%s"), *Suffix);
    ULevelSequence* Sequence = NewObject<ULevelSequence>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("LevelSequence created"), Sequence))
    {
        return true;
    }
    Sequence->Initialize();

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene created by Initialize"), MovieScene))
    {
        return true;
    }

    ULevelSequence* SubSequence = NewObject<ULevelSequence>(
        Package, FName(*SubAssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("SubSequence created"), SubSequence))
    {
        return true;
    }
    SubSequence->Initialize();

    UMovieSceneTrack* Track = MovieScene->AddTrack(UMovieSceneFloatTrack::StaticClass());
    if (!TestNotNull(TEXT("Float track added"), Track))
    {
        return true;
    }
    UMovieSceneSection* Section = Track->CreateNewSection();
    if (!TestNotNull(TEXT("Section created"), Section))
    {
        return true;
    }
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(100)));
    Track->AddSection(*Section);

    UMovieSceneSubTrack* SubTrack = Cast<UMovieSceneSubTrack>(
        MovieScene->AddTrack(UMovieSceneSubTrack::StaticClass()));
    if (!TestNotNull(TEXT("Sub-sequence track added"), SubTrack))
    {
        return true;
    }
    UMovieSceneSubSection* SubSection = SubTrack->AddSequence(SubSequence, FFrameNumber(0), 100);
    if (!TestNotNull(TEXT("Sub-sequence section added"), SubSection))
    {
        return true;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("WrittenPaths contains level_sequence.json"),
        WrittenPathsContains(Result.WrittenPaths, DumpFileNames::LevelSequence));

    const FString SequenceJsonPath = Result.DumpDir / DumpFileNames::LevelSequence;
    TestTrue(TEXT("level_sequence.json exists on disk"),
        IFileManager::Get().FileExists(*SequenceJsonPath));

    TSharedPtr<FJsonObject> Json = LoadJsonFile(SequenceJsonPath);
    if (!TestTrue(TEXT("level_sequence.json parsed"), Json.IsValid()))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    const TSharedPtr<FJsonObject>* TickResolution = nullptr;
    TestTrue(TEXT("tickResolution field present"),
        Json->TryGetObjectField(TEXT("tickResolution"), TickResolution));
    const TSharedPtr<FJsonObject>* DisplayRate = nullptr;
    TestTrue(TEXT("displayRate field present"),
        Json->TryGetObjectField(TEXT("displayRate"), DisplayRate));
    const TSharedPtr<FJsonObject>* PlaybackRange = nullptr;
    TestTrue(TEXT("playbackRange field present"),
        Json->TryGetObjectField(TEXT("playbackRange"), PlaybackRange));

    const TArray<TSharedPtr<FJsonValue>>* SubSequencesArr = nullptr;
    if (TestTrue(TEXT("subSequences field present"), Json->TryGetArrayField(TEXT("subSequences"), SubSequencesArr)))
    {
        TestEqual(TEXT("subSequences length == 1"), SubSequencesArr->Num(), 1);
        if (SubSequencesArr->Num() >= 1)
        {
            TestEqual(TEXT("subSequences contains sub-sequence path"),
                (*SubSequencesArr)[0]->AsString(), SubSequence->GetPathName());
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* TracksArr = nullptr;
    if (TestTrue(TEXT("tracks field present"), Json->TryGetArrayField(TEXT("tracks"), TracksArr)))
    {
        TestEqual(TEXT("tracks length == 2"), TracksArr->Num(), 2);
        if (TracksArr->Num() >= 1)
        {
            const TSharedPtr<FJsonObject>& FirstTrack = (*TracksArr)[0]->AsObject();
            if (TestTrue(TEXT("first track parsed"), FirstTrack.IsValid()))
            {
                double SectionCount = 0.0;
                TestTrue(TEXT("sectionCount present"),
                    FirstTrack->TryGetNumberField(TEXT("sectionCount"), SectionCount));
                TestEqual(TEXT("sectionCount == 1"), static_cast<int32>(SectionCount), 1);

                const TArray<TSharedPtr<FJsonValue>>* SectionsArr = nullptr;
                if (TestTrue(TEXT("sections array present"),
                    FirstTrack->TryGetArrayField(TEXT("sections"), SectionsArr))
                    && SectionsArr->Num() >= 1)
                {
                    const TSharedPtr<FJsonObject>& FirstSection = (*SectionsArr)[0]->AsObject();
                    const TArray<TSharedPtr<FJsonValue>>* ChannelsArr = nullptr;
                    TestTrue(TEXT("channels array present"),
                        FirstSection->TryGetArrayField(TEXT("channels"), ChannelsArr));
                    if (ChannelsArr)
                    {
                        TestTrue(TEXT("channels has at least one entry"), ChannelsArr->Num() >= 1);
                    }
                }
            }
        }
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}
