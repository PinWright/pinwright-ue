// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
// EMovieSceneTransformChannel is declared in MovieSceneSection.h; FMovieSceneTransformMask and
// the section class in the transform section header. Explicit for the -StrictIncludes build.
#include "MovieSceneSection.h"
#include "Sections/MovieScene3DTransformSection.h"

// The nine double channels of a UMovieScene3DTransformSection, shared by every verb that creates,
// masks or reads one. Channel-proxy order is 0-2 Location, 3-5 Rotation, 6-8 Scale on UE 5.3-5.8
// (the proxy always carries all nine; the mask only toggles their editor visibility), and channel
// i's EMovieSceneTransformChannel flag is 1 << i (TranslationX 0x001 ... ScaleZ 0x100). The names
// are the engine's own channel names (UMovieScene3DTransformSection::ChannelOverrideNames).
//
// Why defaults matter: the section's constructor seeds every channel with a DEFAULT (location 0,
// rotation 0, scale 1), and the engine evaluates a channel when the mask enables it AND it holds
// keys OR a default (UMovieScene3DTransformSection::BuildDefaultComponents, 5.8
// MovieScene3DTransformSection.cpp:522-531). An unkeyed channel that kept its constructor default
// therefore pins the bound object to 0 / 1 on that axis. A channel with neither keys nor default
// does not evaluate, and the engine's partial-property path leaves that axis at the object's own
// value.
namespace MovieSceneTransformChannels
{
    constexpr int32 Count = 9;

    inline const TCHAR* Name(int32 Index)
    {
        static const TCHAR* const Names[Count] = {
            TEXT("Location.X"), TEXT("Location.Y"), TEXT("Location.Z"),
            TEXT("Rotation.X"), TEXT("Rotation.Y"), TEXT("Rotation.Z"),
            TEXT("Scale.X"),    TEXT("Scale.Y"),    TEXT("Scale.Z"),
        };
        return Names[Index];
    }

    inline EMovieSceneTransformChannel Flag(int32 Index)
    {
        return static_cast<EMovieSceneTransformChannel>(1u << Index);
    }

    inline TArrayView<FMovieSceneDoubleChannel*> Channels(const UMovieScene3DTransformSection& Section)
    {
        return Section.GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    }

    // Strip the constructor defaults from a section this plugin just created, so only the
    // channels a caller keys ever evaluate. Callers run it on NEW sections only: a default on an
    // existing section may be authored, and sequencer.set_transform_mask is the repair path.
    inline void ClearDefaults(UMovieScene3DTransformSection* Section)
    {
        if (!Section)
        {
            return;
        }
        for (FMovieSceneDoubleChannel* Channel : Channels(*Section))
        {
            if (Channel)
            {
                Channel->ClearDefault();
            }
        }
    }

    // The engine's own per-channel activity test (see the namespace comment).
    inline bool IsChannelEvaluated(const UMovieScene3DTransformSection& Section, int32 Index)
    {
        const TArrayView<FMovieSceneDoubleChannel*> SectionChannels = Channels(Section);
        return EnumHasAnyFlags(Section.GetMask().GetChannels(), Flag(Index)) &&
               SectionChannels.IsValidIndex(Index) && SectionChannels[Index] &&
               SectionChannels[Index]->HasAnyData();
    }

    inline TArray<TSharedPtr<FJsonValue>> MaskToJson(EMovieSceneTransformChannel Mask)
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (EnumHasAnyFlags(Mask, Flag(Index)))
            {
                Names.Add(MakeShared<FJsonValueString>(Name(Index)));
            }
        }
        if (EnumHasAnyFlags(Mask, EMovieSceneTransformChannel::Weight))
        {
            Names.Add(MakeShared<FJsonValueString>(TEXT("Weight")));
        }
        return Names;
    }

    // Readback of a transform section's channel state, merged into the section JSON every
    // sequencer reader and the level_sequence dump emit: transformMask (enabled channels),
    // channelDefaults (name -> default, only channels that carry one) and evaluatedChannels
    // (the channels the engine will actually drive).
    inline void AppendSectionState(const UMovieScene3DTransformSection& Section, const TSharedPtr<FJsonObject>& Obj)
    {
        Obj->SetArrayField(TEXT("transformMask"), MaskToJson(Section.GetMask().GetChannels()));
        const TArrayView<FMovieSceneDoubleChannel*> SectionChannels = Channels(Section);
        TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Evaluated;
        for (int32 Index = 0; Index < Count && Index < SectionChannels.Num(); ++Index)
        {
            const FMovieSceneDoubleChannel* Channel = SectionChannels[Index];
            const TOptional<double> Default = Channel ? Channel->GetDefault() : TOptional<double>();
            if (Default.IsSet())
            {
                Defaults->SetNumberField(Name(Index), Default.GetValue());
            }
            if (IsChannelEvaluated(Section, Index))
            {
                Evaluated.Add(MakeShared<FJsonValueString>(Name(Index)));
            }
        }
        Obj->SetObjectField(TEXT("channelDefaults"), Defaults);
        Obj->SetArrayField(TEXT("evaluatedChannels"), Evaluated);
    }

    // Parse a `channels` list into a mask: each entry is a group (Location | Rotation | Scale)
    // or one channel (Location.X ... Scale.Z), case-insensitive. Refuses an empty list, a
    // non-string entry or an unknown name, naming it, so a typo cannot silently drop a channel.
    inline bool ParseChannelList(const TArray<TSharedPtr<FJsonValue>>& Entries,
                                 EMovieSceneTransformChannel& OutMask, FString& OutError)
    {
        static const TCHAR* const Groups[3] = { TEXT("Location"), TEXT("Rotation"), TEXT("Scale") };
        OutMask = EMovieSceneTransformChannel::None;
        if (Entries.Num() == 0)
        {
            OutError = TEXT("channels is empty; name at least one of Location, Rotation, Scale or "
                            "Location.X ... Scale.Z (to stop a track evaluating, use "
                            "sequencer.set_track_muted or sequencer.remove_track)");
            return false;
        }
        for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
        {
            FString Text;
            if (!Entries[EntryIndex].IsValid() || !Entries[EntryIndex]->TryGetString(Text))
            {
                OutError = FString::Printf(TEXT("channels[%d] is not a string"), EntryIndex);
                return false;
            }
            bool bMatched = false;
            for (int32 Group = 0; Group < 3 && !bMatched; ++Group)
            {
                if (Text.Equals(Groups[Group], ESearchCase::IgnoreCase))
                {
                    OutMask |= Flag(Group * 3) | Flag(Group * 3 + 1) | Flag(Group * 3 + 2);
                    bMatched = true;
                }
            }
            for (int32 Index = 0; Index < Count && !bMatched; ++Index)
            {
                if (Text.Equals(Name(Index), ESearchCase::IgnoreCase))
                {
                    OutMask |= Flag(Index);
                    bMatched = true;
                }
            }
            if (!bMatched)
            {
                OutError = FString::Printf(TEXT("channels[%d] '%s' is not a transform channel (accepted: "
                    "Location, Rotation, Scale, Location.X|Y|Z, Rotation.X|Y|Z, Scale.X|Y|Z; "
                    "Rotation.X = roll, Rotation.Y = pitch, Rotation.Z = yaw)"), EntryIndex, *Text);
                return false;
            }
        }
        return true;
    }
}
