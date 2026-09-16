// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/WidgetAnimationEventIntrospection.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationDelegateBinding.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Channels/MovieSceneEvent.h"
#include "Channels/MovieSceneEventChannel.h"
#include "EdGraph/EdGraph.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_Event.h"
#include "K2Node_WidgetAnimationEvent.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Sections/MovieSceneEventRepeaterSection.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "WidgetBlueprint.h"

namespace
{
    FString WidgetAnimationEventToString(EWidgetAnimationEvent Action)
    {
        switch (Action)
        {
        case EWidgetAnimationEvent::Started:
            return TEXT("Started");
        case EWidgetAnimationEvent::Finished:
            return TEXT("Finished");
        default:
            return TEXT("Unknown");
        }
    }

    bool MatchesAnimationName(const FName& Candidate, const UWidgetAnimation* Animation)
    {
        return Animation
            && !Candidate.IsNone()
            && (Candidate == Animation->GetFName()
                || Candidate.ToString().Equals(Animation->GetName(), ESearchCase::IgnoreCase));
    }

    FString GetEndpointName(const FMovieSceneEvent& Event)
    {
        if (UObject* Endpoint = Event.WeakEndpoint.Get())
        {
            if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Endpoint))
            {
                const FName FunctionName = EventNode->GetFunctionName();
                if (!FunctionName.IsNone())
                {
                    return FunctionName.ToString();
                }
            }
            return Endpoint->GetName();
        }
        return FString();
    }

    FString GetEventFunctionName(const FMovieSceneEvent& Event)
    {
        if (Event.Ptrs.Function)
        {
            return Event.Ptrs.Function->GetName();
        }
        if (!Event.CompiledFunctionName.IsNone())
        {
            return Event.CompiledFunctionName.ToString();
        }
        return GetEndpointName(Event);
    }

    TSharedPtr<FJsonObject> MakeEventObject(
        const FMovieSceneEvent& Event,
        const FFrameRate& TickResolution,
        const FString& SectionType,
        int32 SectionIndex,
        int32 EventIndex,
        TOptional<FFrameNumber> FrameNumber,
        TSet<FString>& TriggeredFunctions)
    {
        TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
        EventObj->SetStringField(TEXT("sectionType"), SectionType);
        EventObj->SetNumberField(TEXT("sectionIndex"), SectionIndex);
        EventObj->SetNumberField(TEXT("eventIndex"), EventIndex);

        if (FrameNumber.IsSet())
        {
            EventObj->SetNumberField(TEXT("frame"), FrameNumber.GetValue().Value);
            EventObj->SetNumberField(TEXT("time"), TickResolution.AsSeconds(FrameNumber.GetValue()));
        }

        const FString FunctionName = GetEventFunctionName(Event);
        if (!FunctionName.IsEmpty())
        {
            EventObj->SetStringField(TEXT("functionName"), FunctionName);
            TriggeredFunctions.Add(FunctionName);
        }

        if (!Event.CompiledFunctionName.IsNone())
        {
            EventObj->SetStringField(TEXT("compiledFunctionName"), Event.CompiledFunctionName.ToString());
        }
        const FString EndpointName = GetEndpointName(Event);
        if (!EndpointName.IsEmpty())
        {
            EventObj->SetStringField(TEXT("endpointName"), EndpointName);
        }

        return EventObj;
    }

    void AppendSectionEvents(
        UMovieSceneSection* Section,
        const FFrameRate& TickResolution,
        int32 SectionIndex,
        TArray<TSharedPtr<FJsonValue>>& OutEvents,
        TSet<FString>& TriggeredFunctions)
    {
        if (!Section)
        {
            return;
        }

        const FString SectionType = Section->GetClass()->GetName();
        if (UMovieSceneEventTriggerSection* TriggerSection = Cast<UMovieSceneEventTriggerSection>(Section))
        {
            TMovieSceneChannelData<const FMovieSceneEvent> Data = TriggerSection->EventChannel.GetData();
            const TArrayView<const FFrameNumber> Times = Data.GetTimes();
            const TArrayView<const FMovieSceneEvent> Events = Data.GetValues();
            for (int32 EventIndex = 0; EventIndex < Events.Num(); ++EventIndex)
            {
                const TOptional<FFrameNumber> FrameNumber = Times.IsValidIndex(EventIndex)
                    ? TOptional<FFrameNumber>(Times[EventIndex])
                    : TOptional<FFrameNumber>();
                OutEvents.Add(MakeShared<FJsonValueObject>(
                    MakeEventObject(Events[EventIndex], TickResolution, SectionType, SectionIndex, EventIndex, FrameNumber, TriggeredFunctions)));
            }
            return;
        }

        if (UMovieSceneEventRepeaterSection* RepeaterSection = Cast<UMovieSceneEventRepeaterSection>(Section))
        {
            TOptional<FFrameNumber> FrameNumber;
            if (RepeaterSection->GetRange().HasLowerBound())
            {
                FrameNumber = RepeaterSection->GetRange().GetLowerBoundValue();
            }
            OutEvents.Add(MakeShared<FJsonValueObject>(
                MakeEventObject(RepeaterSection->Event, TickResolution, SectionType, SectionIndex, 0, FrameNumber, TriggeredFunctions)));
            return;
        }

        if (UMovieSceneEventSectionBase* EventSection = Cast<UMovieSceneEventSectionBase>(Section))
        {
            const TArrayView<FMovieSceneEvent> EntryPoints = EventSection->GetAllEntryPoints();
            for (int32 EventIndex = 0; EventIndex < EntryPoints.Num(); ++EventIndex)
            {
                OutEvents.Add(MakeShared<FJsonValueObject>(
                    MakeEventObject(EntryPoints[EventIndex], TickResolution, SectionType, SectionIndex, EventIndex, TOptional<FFrameNumber>(), TriggeredFunctions)));
            }
        }
    }

    void AppendEventTrackObject(
        UMovieSceneEventTrack* EventTrack,
        const FFrameRate& TickResolution,
        const FString& Scope,
        const FString& BindingGuid,
        const FString& BindingName,
        TArray<TSharedPtr<FJsonValue>>& EventTracks,
        TSet<FString>& TriggeredFunctions)
    {
        if (!EventTrack)
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Events;
        const TArray<UMovieSceneSection*>& Sections = EventTrack->GetAllSections();
        for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
        {
            AppendSectionEvents(Sections[SectionIndex], TickResolution, SectionIndex, Events, TriggeredFunctions);
        }

        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("scope"), Scope);
        TrackObj->SetStringField(TEXT("name"), EventTrack->GetTrackName().ToString());
        TrackObj->SetStringField(TEXT("type"), EventTrack->GetClass()->GetName());
        TrackObj->SetNumberField(TEXT("sectionCount"), Sections.Num());
        TrackObj->SetArrayField(TEXT("events"), Events);
        TrackObj->SetNumberField(TEXT("eventCount"), Events.Num());

        if (!BindingGuid.IsEmpty())
        {
            TrackObj->SetStringField(TEXT("bindingGuid"), BindingGuid);
        }
        if (!BindingName.IsEmpty())
        {
            TrackObj->SetStringField(TEXT("bindingName"), BindingName);
        }

        EventTracks.Add(MakeShared<FJsonValueObject>(TrackObj));
    }

    void AppendEventTracks(
        UWidgetAnimation* Animation,
        TArray<TSharedPtr<FJsonValue>>& EventTracks,
        TSet<FString>& TriggeredFunctions)
    {
        UMovieScene* MovieScene = Animation ? Animation->GetMovieScene() : nullptr;
        if (!MovieScene)
        {
            return;
        }

        const FFrameRate TickResolution = MovieScene->GetTickResolution();
        for (UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            AppendEventTrackObject(Cast<UMovieSceneEventTrack>(Track), TickResolution, TEXT("animation"), FString(), FString(), EventTracks, TriggeredFunctions);
        }

        for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            FString BindingName;
            if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Binding.GetObjectGuid()))
            {
                BindingName = Possessable->GetName();
            }
            else if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Binding.GetObjectGuid()))
            {
                BindingName = Spawnable->GetName();
            }

            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                AppendEventTrackObject(
                    Cast<UMovieSceneEventTrack>(Track),
                    TickResolution,
                    TEXT("binding"),
                    Binding.GetObjectGuid().ToString(),
                    BindingName,
                    EventTracks,
                    TriggeredFunctions);
            }
        }
    }

    void AppendDelegateBindingObject(
        const FString& Source,
        const FString& FunctionName,
        EWidgetAnimationEvent Action,
        const FName& UserTag,
        const FString& GraphName,
        TArray<TSharedPtr<FJsonValue>>& DelegateBindings,
        TSet<FString>& DelegateKeys,
        TSet<FString>& TriggeredFunctions)
    {
        if (FunctionName.IsEmpty())
        {
            return;
        }

        const FString Key = FString::Printf(TEXT("%s|%s|%s|%s"),
            *Source,
            *WidgetAnimationEventToString(Action),
            *FunctionName,
            *UserTag.ToString());
        if (DelegateKeys.Contains(Key))
        {
            return;
        }
        DelegateKeys.Add(Key);
        TriggeredFunctions.Add(FunctionName);

        TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
        BindingObj->SetStringField(TEXT("source"), Source);
        BindingObj->SetStringField(TEXT("action"), WidgetAnimationEventToString(Action));
        BindingObj->SetStringField(TEXT("functionName"), FunctionName);
        if (!UserTag.IsNone())
        {
            BindingObj->SetStringField(TEXT("userTag"), UserTag.ToString());
        }
        if (!GraphName.IsEmpty())
        {
            BindingObj->SetStringField(TEXT("graphName"), GraphName);
        }
        DelegateBindings.Add(MakeShared<FJsonValueObject>(BindingObj));
    }

    void AppendGeneratedClassDelegateBindings(
        UWidgetBlueprint* WidgetBlueprint,
        UWidgetAnimation* Animation,
        TArray<TSharedPtr<FJsonValue>>& DelegateBindings,
        TSet<FString>& DelegateKeys,
        TSet<FString>& TriggeredFunctions)
    {
        UBlueprintGeneratedClass* GeneratedClass = WidgetBlueprint ? Cast<UBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass) : nullptr;
        if (!GeneratedClass)
        {
            return;
        }

        for (UDynamicBlueprintBinding* BindingObject : GeneratedClass->DynamicBindingObjects)
        {
            UWidgetAnimationDelegateBinding* AnimationBinding = Cast<UWidgetAnimationDelegateBinding>(BindingObject);
            if (!AnimationBinding)
            {
                continue;
            }

            for (const FBlueprintWidgetAnimationDelegateBinding& Binding : AnimationBinding->WidgetAnimationDelegateBindings)
            {
                if (!MatchesAnimationName(Binding.AnimationToBind, Animation))
                {
                    continue;
                }

                AppendDelegateBindingObject(
                    TEXT("generatedClass"),
                    Binding.FunctionNameToBind.ToString(),
                    Binding.Action,
                    Binding.UserTag,
                    FString(),
                    DelegateBindings,
                    DelegateKeys,
                    TriggeredFunctions);
            }
        }
    }

    void AppendGraphNodeDelegateBindings(
        UWidgetBlueprint* WidgetBlueprint,
        UWidgetAnimation* Animation,
        TArray<TSharedPtr<FJsonValue>>& DelegateBindings,
        TSet<FString>& DelegateKeys,
        TSet<FString>& TriggeredFunctions)
    {
        if (!WidgetBlueprint)
        {
            return;
        }

        for (UEdGraph* Graph : WidgetBlueprint->UbergraphPages)
        {
            if (!Graph)
            {
                continue;
            }

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_WidgetAnimationEvent* AnimationEventNode = Cast<UK2Node_WidgetAnimationEvent>(Node);
                if (!AnimationEventNode || !MatchesAnimationName(AnimationEventNode->AnimationPropertyName, Animation))
                {
                    continue;
                }

                AppendDelegateBindingObject(
                    TEXT("graphNode"),
                    AnimationEventNode->GetFunctionName().ToString(),
                    AnimationEventNode->Action,
                    AnimationEventNode->UserTag,
                    Graph->GetName(),
                    DelegateBindings,
                    DelegateKeys,
                    TriggeredFunctions);
            }
        }
    }
}

namespace WidgetAnimationEventIntrospection
{
    void AppendAnimationEventMetadata(
        UWidgetBlueprint* WidgetBlueprint,
        UWidgetAnimation* Animation,
        const TSharedPtr<FJsonObject>& AnimationObject)
    {
        if (!AnimationObject.IsValid())
        {
            return;
        }

        TSet<FString> TriggeredFunctions;
        TSet<FString> DelegateKeys;
        TArray<TSharedPtr<FJsonValue>> EventTracks;
        TArray<TSharedPtr<FJsonValue>> DelegateBindings;

        AppendEventTracks(Animation, EventTracks, TriggeredFunctions);
        AppendGeneratedClassDelegateBindings(WidgetBlueprint, Animation, DelegateBindings, DelegateKeys, TriggeredFunctions);
        AppendGraphNodeDelegateBindings(WidgetBlueprint, Animation, DelegateBindings, DelegateKeys, TriggeredFunctions);

        TArray<FString> SortedFunctions = TriggeredFunctions.Array();
        SortedFunctions.Sort();
        TArray<TSharedPtr<FJsonValue>> TriggeredFunctionValues;
        for (const FString& FunctionName : SortedFunctions)
        {
            TriggeredFunctionValues.Add(MakeShared<FJsonValueString>(FunctionName));
        }

        AnimationObject->SetArrayField(TEXT("eventTracks"), EventTracks);
        AnimationObject->SetArrayField(TEXT("delegateBindings"), DelegateBindings);
        AnimationObject->SetArrayField(TEXT("triggeredFunctions"), TriggeredFunctionValues);
        AnimationObject->SetNumberField(TEXT("eventTrackCount"), EventTracks.Num());
        AnimationObject->SetNumberField(TEXT("delegateBindingCount"), DelegateBindings.Num());
        AnimationObject->SetNumberField(TEXT("triggeredFunctionCount"), TriggeredFunctionValues.Num());
    }
}
