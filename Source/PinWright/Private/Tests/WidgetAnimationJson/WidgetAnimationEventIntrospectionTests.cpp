// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetAnimationJsonTestUtils.h"

#include "Animation/WidgetAnimationDelegateBinding.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Channels/MovieSceneEvent.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Dom/JsonObject.h"
#include "MovieScene.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "WidgetBlueprint.h"


namespace
{
    TSharedPtr<FJsonObject> MakeWidgetAnimationEventPayload(
        const FString& WidgetPath,
        const FString& AnimationName,
        bool bIncludeEvents)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        Payload->SetStringField(TEXT("animationName"), AnimationName);
        Payload->SetBoolField(TEXT("includeEvents"), bIncludeEvents);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeWidgetAnimationExportPayload(
        const FString& WidgetPath,
        bool bIncludeEventMetadata)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        Payload->SetBoolField(TEXT("includeEventMetadata"), bIncludeEventMetadata);
        return Payload;
    }

    bool AddSequenceEventTrack(UMovieScene* MovieScene)
    {
        if (!MovieScene)
        {
            return false;
        }

        UMovieSceneEventTrack* EventTrack = MovieScene->AddTrack<UMovieSceneEventTrack>();
        UMovieSceneEventTriggerSection* TriggerSection = EventTrack
            ? Cast<UMovieSceneEventTriggerSection>(EventTrack->CreateNewSection())
            : nullptr;
        if (!EventTrack || !TriggerSection)
        {
            return false;
        }

        TriggerSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));

        FMovieSceneEvent Event;
        Event.CompiledFunctionName = FName(TEXT("SequenceEvent"));
        TriggerSection->EventChannel.GetData().AddKey(FFrameNumber(12), Event);
        EventTrack->AddSection(*TriggerSection);
        return true;
    }

    bool AddGeneratedClassDelegateBinding(UWidgetBlueprint* WidgetBlueprint)
    {
        if (!WidgetBlueprint)
        {
            return false;
        }

        UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass);
        if (!GeneratedClass)
        {
            // UWidgetBlueprintGeneratedClass derives from UClass whose ClassWithin is UPackage,
            // so the Outer must be the WidgetBlueprint's package, not the WidgetBlueprint asset.
            // Passing the WBP triggers a CoreUObject ensure and produces a malformed UClass that
            // does not surface its DynamicBindingObjects to introspection callers.
            UPackage* Package = WidgetBlueprint->GetOutermost();
            if (!Package)
            {
                return false;
            }
            // The synthetic UClass needs to be a fully linked stub before any UObject can be
            // safely outered to it. Without StaticLink the class layout is incomplete and
            // child UObjects (like UDynamicBlueprintBinding) added via NewObject may be culled
            // by GC before the introspection handler reads back DynamicBindingObjects.
            GeneratedClass = NewObject<UWidgetBlueprintGeneratedClass>(
                Package,
                FName(TEXT("WBP_EventIntrospection_C")),
                RF_Transient | RF_Public | RF_Standalone);
            if (GeneratedClass)
            {
                GeneratedClass->SetSuperStruct(UUserWidget::StaticClass());
                GeneratedClass->ClassWithin = UObject::StaticClass();
                GeneratedClass->ClassConfigName = UUserWidget::StaticClass()->ClassConfigName;
                GeneratedClass->Bind();
                GeneratedClass->StaticLink(true);
                GeneratedClass->AssembleReferenceTokenStream();
                GeneratedClass->GetDefaultObject();
            }
            WidgetBlueprint->GeneratedClass = GeneratedClass;
        }
        if (!GeneratedClass)
        {
            return false;
        }

        UWidgetAnimationDelegateBinding* BindingObject = NewObject<UWidgetAnimationDelegateBinding>(
            GeneratedClass,
            NAME_None,
            RF_Transient | RF_Public | RF_Standalone);
        if (!BindingObject)
        {
            return false;
        }

        FBlueprintWidgetAnimationDelegateBinding Binding;
        Binding.AnimationToBind = FName(TEXT("FadeIn"));
        Binding.Action = EWidgetAnimationEvent::Finished;
        Binding.FunctionNameToBind = FName(TEXT("SequenceEvent"));
        BindingObject->WidgetAnimationDelegateBindings.Add(Binding);
        GeneratedClass->DynamicBindingObjects.Add(BindingObject);
        return true;
    }

    TSharedPtr<FJsonObject> GetFirstAnimationFromExport(const FTestResponseCapture& Capture)
    {
        if (!Capture.Result.IsValid())
        {
            return nullptr;
        }

        const TSharedPtr<FJsonObject>* Document = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("json"), Document) || !Document || !Document->IsValid())
        {
            return nullptr;
        }

        const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
        if (!(*Document)->TryGetArrayField(TEXT("animations"), Animations) || !Animations || Animations->Num() == 0)
        {
            return nullptr;
        }

        const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
        if (!(*Animations)[0]->TryGetObject(AnimationObj) || !AnimationObj || !AnimationObj->IsValid())
        {
            return nullptr;
        }
        return *AnimationObj;
    }

    bool FirstEventFunctionNameEquals(const TSharedPtr<FJsonObject>& AnimationObject, const FString& ExpectedFunctionName)
    {
        const TArray<TSharedPtr<FJsonValue>>* EventTracks = nullptr;
        if (!AnimationObject.IsValid() || !AnimationObject->TryGetArrayField(TEXT("eventTracks"), EventTracks) || !EventTracks || EventTracks->Num() == 0)
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* EventTrackObj = nullptr;
        if (!(*EventTracks)[0]->TryGetObject(EventTrackObj) || !EventTrackObj || !EventTrackObj->IsValid())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        if (!(*EventTrackObj)->TryGetArrayField(TEXT("events"), Events) || !Events || Events->Num() == 0)
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* EventObj = nullptr;
        if (!(*Events)[0]->TryGetObject(EventObj) || !EventObj || !EventObj->IsValid())
        {
            return false;
        }

        FString FunctionName;
        return (*EventObj)->TryGetStringField(TEXT("functionName"), FunctionName)
            && FunctionName == ExpectedFunctionName;
    }

    bool HasDelegateBinding(
        const TSharedPtr<FJsonObject>& AnimationObject,
        const FString& ExpectedAction,
        const FString& ExpectedFunctionName)
    {
        const TArray<TSharedPtr<FJsonValue>>* DelegateBindings = nullptr;
        if (!AnimationObject.IsValid() || !AnimationObject->TryGetArrayField(TEXT("delegateBindings"), DelegateBindings) || !DelegateBindings)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& DelegateValue : *DelegateBindings)
        {
            const TSharedPtr<FJsonObject>* DelegateObj = nullptr;
            if (!DelegateValue.IsValid() || !DelegateValue->TryGetObject(DelegateObj) || !DelegateObj || !DelegateObj->IsValid())
            {
                continue;
            }

            FString Action;
            FString FunctionName;
            (*DelegateObj)->TryGetStringField(TEXT("action"), Action);
            (*DelegateObj)->TryGetStringField(TEXT("functionName"), FunctionName);
            if (Action == ExpectedAction && FunctionName == ExpectedFunctionName)
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationEventMetadataIntrospectionTest,
    "PinWright.widget.animation_events.EventMetadataIntrospection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationEventMetadataIntrospectionTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    const FWidgetAnimationFixture Fixture = CreateFloatAnimationFixture(TEXT("WBP_WidgetAnimationEventIntrospection"));
    TestNotNull(TEXT("fixture widget blueprint created"), Fixture.WidgetBlueprint);
    TestNotNull(TEXT("fixture movie scene created"), Fixture.MovieScene);
    if (!Fixture.WidgetBlueprint || !Fixture.MovieScene)
    {
        return false;
    }

    TestTrue(TEXT("root event track added"), AddSequenceEventTrack(Fixture.MovieScene));
    TestTrue(TEXT("generated delegate binding added"), AddGeneratedClassDelegateBinding(Fixture.WidgetBlueprint));

    FTestResponseCapture InfoCapture;
    const bool bInfoFound = InvokeHandlerWithCapture(
        TEXT("widget.get_animation_info"),
        MakeWidgetAnimationEventPayload(Fixture.WidgetPath, TEXT("FadeIn"), true),
        InfoCapture);
    TestTrue(TEXT("get_animation_info handler found"), bInfoFound);
    TestTrue(TEXT("get_animation_info succeeds"), InfoCapture.bSuccess);
    TestTrue(TEXT("get_animation_info event metadata includes SequenceEvent"),
        FirstEventFunctionNameEquals(InfoCapture.Result, TEXT("SequenceEvent")));
    TestTrue(TEXT("get_animation_info delegate metadata includes finished SequenceEvent binding"),
        HasDelegateBinding(InfoCapture.Result, TEXT("Finished"), TEXT("SequenceEvent")));

    FTestResponseCapture ExportCapture;
    const bool bExportFound = InvokeHandlerWithCapture(
        TEXT("widget.export_animations_json"),
        MakeWidgetAnimationExportPayload(Fixture.WidgetPath, true),
        ExportCapture);
    TestTrue(TEXT("export_animations_json handler found"), bExportFound);
    TestTrue(TEXT("export_animations_json succeeds"), ExportCapture.bSuccess);

    TSharedPtr<FJsonObject> ExportedAnimation = GetFirstAnimationFromExport(ExportCapture);
    TestTrue(TEXT("exported animation exists"), ExportedAnimation.IsValid());
    TestTrue(TEXT("exported event metadata includes SequenceEvent"),
        FirstEventFunctionNameEquals(ExportedAnimation, TEXT("SequenceEvent")));
    TestTrue(TEXT("exported delegate metadata includes finished SequenceEvent binding"),
        HasDelegateBinding(ExportedAnimation, TEXT("Finished"), TEXT("SequenceEvent")));

    return true;
}
