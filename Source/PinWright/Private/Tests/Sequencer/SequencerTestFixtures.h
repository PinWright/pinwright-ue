// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared LevelSequence construction helpers used by sequencer tests.
// Inline so the same implementation is available across translation units
// without an extra .cpp (mirrors Tests/Widget/WidgetTestFixtures.h).

#include "CoreMinimal.h"
#include "LevelSequence.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace SequencerTestFixtures
{
    // Invokes Visitor for each per-track JSON object in a sequencer.list_tracks
    // response (the tracks[] array). Validates the envelope and skips malformed
    // entries, so callers express their per-entry predicate without re-walking
    // the array scaffold.
    inline void ForEachTrackEntry(const TSharedPtr<FJsonObject>& Result,
        TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Visitor)
    {
        if (!Result.IsValid())
        {
            return;
        }
        const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
        if (!Result->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Val : *Tracks)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Val.IsValid() && Val->TryGetObject(Entry) && Entry && (*Entry).IsValid())
            {
                Visitor(*Entry);
            }
        }
    }

    // Returns the first per-track JSON object whose trackName equals TrackName,
    // or nullptr if none. Built on ForEachTrackEntry.
    inline TSharedPtr<FJsonObject> FindTrackEntryByName(
        const TSharedPtr<FJsonObject>& Result, const FString& TrackName)
    {
        TSharedPtr<FJsonObject> Found;
        ForEachTrackEntry(Result, [&](const TSharedPtr<FJsonObject>& Entry)
        {
            FString Name;
            if (!Found.IsValid()
                && Entry->TryGetStringField(TEXT("trackName"), Name) && Name == TrackName)
            {
                Found = Entry;
            }
        });
        return Found;
    }

    // Builds a transient ULevelSequence in /Engine/Transient/<Prefix>_<unique> and
    // returns (sequence, fully-qualified object path). Returns nullptr on failure.
    inline ULevelSequence* MakeTransientSequence(const FString& Prefix, FString& OutObjectPath)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/%s_%s"), *Prefix, *Suffix);
        const FString AssetName   = FString::Printf(TEXT("%s_%s"), *Prefix, *Suffix);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        ULevelSequence* Sequence = NewObject<ULevelSequence>(
            Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
        if (!Sequence)
        {
            return nullptr;
        }
        Sequence->Initialize();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
        return Sequence;
    }
}
