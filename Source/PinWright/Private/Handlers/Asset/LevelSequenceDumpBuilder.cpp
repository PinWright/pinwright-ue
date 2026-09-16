// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/LevelSequenceDumpBuilder.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneSubSection.h"
#include "UObject/Class.h"
#include "Utils/MovieSceneJsonUtils.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    using MovieSceneJsonUtils::MakeFrameRateObject;
    using MovieSceneJsonUtils::MakeFrameRangeObject;
    using MovieSceneJsonUtils::BuildTrackJson;

    TArray<TSharedPtr<FJsonValue>> BuildSubSequencesJson(const UMovieScene* MovieScene)
    {
        TSet<FString> UniquePaths;
        for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            if (!Track)
            {
                continue;
            }

            for (const UMovieSceneSection* Section : Track->GetAllSections())
            {
                const UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section);
                if (!SubSection)
                {
                    continue;
                }

                const UMovieSceneSequence* SubSequence = SubSection->GetSequence();
                if (SubSequence)
                {
                    UniquePaths.Add(SubSequence->GetPathName());
                }
            }
        }

        TArray<FString> SortedPaths = UniquePaths.Array();
        SortedPaths.Sort();

        TArray<TSharedPtr<FJsonValue>> SubSequences;
        SubSequences.Reserve(SortedPaths.Num());
        for (const FString& Path : SortedPaths)
        {
            SubSequences.Add(MakeShared<FJsonValueString>(Path));
        }
        return SubSequences;
    }
}

TSharedPtr<FJsonObject> LevelSequenceDumpBuilder::BuildLevelSequenceJson(const ULevelSequence* Sequence)
{
    if (!Sequence)
    {
        return nullptr;
    }
    const UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetObjectField(TEXT("tickResolution"), MakeFrameRateObject(MovieScene->GetTickResolution()));
    Root->SetObjectField(TEXT("displayRate"), MakeFrameRateObject(MovieScene->GetDisplayRate()));
    Root->SetObjectField(TEXT("playbackRange"), MakeFrameRangeObject(MovieScene->GetPlaybackRange()));

    const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
    Root->SetNumberField(TEXT("bindingCount"), Bindings.Num());
    Root->SetNumberField(TEXT("spawnableCount"), MovieScene->GetSpawnableCount());
    Root->SetNumberField(TEXT("possessableCount"), MovieScene->GetPossessableCount());

    TArray<TSharedPtr<FJsonValue>> TrackArr;
    for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        TrackArr.Add(MakeShared<FJsonValueObject>(BuildTrackJson(Track).ToSharedRef()));
    }
    Root->SetArrayField(TEXT("tracks"), TrackArr);
    Root->SetArrayField(TEXT("subSequences"), BuildSubSequencesJson(MovieScene));

    if (UMovieSceneTrack* CameraCut = MovieScene->GetCameraCutTrack())
    {
        Root->SetObjectField(TEXT("cameraCutTrack"), BuildTrackJson(CameraCut));
    }
    else
    {
        Root->SetField(TEXT("cameraCutTrack"), MakeShared<FJsonValueNull>());
    }

    // Sort bindings by guid for deterministic, diff-stable output.
    TArray<const FMovieSceneBinding*> SortedBindings;
    SortedBindings.Reserve(Bindings.Num());
    for (const FMovieSceneBinding& Binding : Bindings)
    {
        SortedBindings.Add(&Binding);
    }
    SortedBindings.Sort([](const FMovieSceneBinding& A, const FMovieSceneBinding& B)
    {
        return A.GetObjectGuid() < B.GetObjectGuid();
    });

    // FindPossessable/FindSpawnable are non-const; resolve binding names through the owning scene.
    UMovieScene* MutableMovieScene = Sequence->GetMovieScene();

    TArray<TSharedPtr<FJsonValue>> BindingArr;
    for (const FMovieSceneBinding* Binding : SortedBindings)
    {
        TSharedRef<FJsonObject> BindingObj = MakeShared<FJsonObject>();
        BindingObj->SetStringField(TEXT("guid"), Binding->GetObjectGuid().ToString());

        FString BindingName;
        if (const FMovieScenePossessable* Possessable = MutableMovieScene->FindPossessable(Binding->GetObjectGuid()))
        {
            BindingName = Possessable->GetName();
        }
        else if (const FMovieSceneSpawnable* Spawnable = MutableMovieScene->FindSpawnable(Binding->GetObjectGuid()))
        {
            BindingName = Spawnable->GetName();
        }
        BindingObj->SetStringField(TEXT("name"), BindingName);

        TArray<TSharedPtr<FJsonValue>> BindingTrackArr;
        for (const UMovieSceneTrack* Track : Binding->GetTracks())
        {
            BindingTrackArr.Add(MakeShared<FJsonValueObject>(BuildTrackJson(Track).ToSharedRef()));
        }
        BindingObj->SetArrayField(TEXT("tracks"), BindingTrackArr);
        BindingArr.Add(MakeShared<FJsonValueObject>(BindingObj));
    }
    Root->SetArrayField(TEXT("bindings"), BindingArr);

    return Root;
}

namespace
{
    UClass* GetLevelSequenceSidecarClass()
    {
        return ULevelSequence::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildLevelSequenceSidecar(UObject* Asset)
    {
        return LevelSequenceDumpBuilder::BuildLevelSequenceJson(Cast<ULevelSequence>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("level_sequence"), DumpFileNames::LevelSequence,
    &GetLevelSequenceSidecarClass, &BuildLevelSequenceSidecar,
    nullptr, nullptr, nullptr, 100);
