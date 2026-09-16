// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetAnimationJsonTestUtils.h"

#include "Handlers/UI/WidgetAnimationJsonSerializer.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/SlateWrapperTypes.h"
#include "Dom/JsonObject.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
// The MovieSceneText headers moved into Channels/Sections/Tracks subfolders (MovieSceneTracks
// module) in UE 5.7; the old flat paths remain only as deprecation stubs. Prefer the new
// locations when present, fall back to the flat form for UE 5.6 and earlier.
#if __has_include("Channels/MovieSceneTextChannel.h")
#include "Channels/MovieSceneTextChannel.h"
#else
#include "MovieSceneTextChannel.h"
#endif
#if __has_include("Sections/MovieSceneTextSection.h")
#include "Sections/MovieSceneTextSection.h"
#else
#include "MovieSceneTextSection.h"
#endif
#if __has_include("Tracks/MovieSceneTextTrack.h")
#include "Tracks/MovieSceneTextTrack.h"
#else
#include "MovieSceneTextTrack.h"
#endif
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonExportRegisteredTest,
    "PinWright.widget.export_animations_json.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonExportRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("widget.export_animations_json is registered"),
        IsHandlerRegistered(TEXT("widget.export_animations_json")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonImportRegisteredTest,
    "PinWright.widget.import_animations_json.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonImportRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("widget.import_animations_json is registered"),
        IsHandlerRegistered(TEXT("widget.import_animations_json")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonExportEmptyParamsNoCrashTest,
    "PinWright.widget.export_animations_json.EmptyParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonExportEmptyParamsNoCrashTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("widget.export_animations_json"),
        MakeShared<FJsonObject>(),
        Capture);
    TestTrue(TEXT("handler found"), bFound);
    if (bFound)
    {
        TestFalse(TEXT("should fail with empty params"), Capture.bSuccess);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonImportEmptyParamsNoCrashTest,
    "PinWright.widget.import_animations_json.EmptyParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonImportEmptyParamsNoCrashTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("widget.import_animations_json"),
        MakeShared<FJsonObject>(),
        Capture);
    TestTrue(TEXT("handler found"), bFound);
    if (bFound)
    {
        TestFalse(TEXT("should fail with empty params"), Capture.bSuccess);
    }
    return true;
}


namespace
{
    TSharedPtr<FJsonObject> GetExportedJsonDocument(const FTestResponseCapture& Capture)
    {
        if (!Capture.Result.IsValid())
        {
            return nullptr;
        }

        const TSharedPtr<FJsonObject>* Document = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("json"), Document) || !Document)
        {
            return nullptr;
        }
        return *Document;
    }

    TSharedPtr<FJsonObject> MakePayloadWithWidgetPath(const FString& WidgetPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeFrameRateObject(int32 Numerator, int32 Denominator)
    {
        TSharedPtr<FJsonObject> Rate = MakeShared<FJsonObject>();
        Rate->SetNumberField(TEXT("numerator"), Numerator);
        Rate->SetNumberField(TEXT("denominator"), Denominator);
        return Rate;
    }

    TSharedPtr<FJsonObject> MakeFrameRangeObject(int32 StartFrame, int32 EndFrame)
    {
        TSharedPtr<FJsonObject> Range = MakeShared<FJsonObject>();
        Range->SetNumberField(TEXT("startFrame"), StartFrame);
        Range->SetNumberField(TEXT("endFrame"), EndFrame);
        return Range;
    }

    TSharedPtr<FJsonObject> MakeMinimalAnimationDocument(const FString& WidgetName, const FString& AnimationName)
    {
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("time"), 0.5);
        Key->SetNumberField(TEXT("value"), 0.25);

        TArray<TSharedPtr<FJsonValue>> Keys;
        Keys.Add(MakeShared<FJsonValueObject>(Key));

        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetObjectField(TEXT("range"), MakeFrameRangeObject(0, 30));
        Section->SetArrayField(TEXT("keys"), Keys);

        TArray<TSharedPtr<FJsonValue>> Sections;
        Sections.Add(MakeShared<FJsonValueObject>(Section));

        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("type"), TEXT("float"));
        Track->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
        Track->SetStringField(TEXT("propertyPath"), TEXT("RenderOpacity"));
        Track->SetArrayField(TEXT("sections"), Sections);

        TArray<TSharedPtr<FJsonValue>> Tracks;
        Tracks.Add(MakeShared<FJsonValueObject>(Track));

        TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
        Binding->SetStringField(TEXT("widgetName"), WidgetName);
        Binding->SetArrayField(TEXT("tracks"), Tracks);

        TArray<TSharedPtr<FJsonValue>> Bindings;
        Bindings.Add(MakeShared<FJsonValueObject>(Binding));

        TSharedPtr<FJsonObject> Animation = MakeShared<FJsonObject>();
        Animation->SetStringField(TEXT("name"), AnimationName);
        Animation->SetObjectField(TEXT("displayRate"), MakeFrameRateObject(30, 1));
        Animation->SetObjectField(TEXT("tickResolution"), MakeFrameRateObject(30, 1));
        Animation->SetObjectField(TEXT("playbackRange"), MakeFrameRangeObject(0, 30));
        Animation->SetArrayField(TEXT("bindings"), Bindings);

        TArray<TSharedPtr<FJsonValue>> Animations;
        Animations.Add(MakeShared<FJsonValueObject>(Animation));

        TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
        Document->SetStringField(TEXT("schema"), TEXT("pinwright.widget-animations.v1"));
        Document->SetArrayField(TEXT("animations"), Animations);
        return Document;
    }

    TSharedPtr<FJsonObject> GetFirstBinding(TSharedPtr<FJsonObject> Document)
    {
        const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
        if (!Document.IsValid() || !Document->TryGetArrayField(TEXT("animations"), Animations) || !Animations || Animations->Num() == 0)
        {
            return nullptr;
        }

        const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
        if (!(*Animations)[0]->TryGetObject(AnimationObj) || !AnimationObj || !AnimationObj->IsValid())
        {
            return nullptr;
        }

        const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
        if (!(*AnimationObj)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings || Bindings->Num() == 0)
        {
            return nullptr;
        }

        const TSharedPtr<FJsonObject>* BindingObj = nullptr;
        if (!(*Bindings)[0]->TryGetObject(BindingObj) || !BindingObj || !BindingObj->IsValid())
        {
            return nullptr;
        }
        return *BindingObj;
    }

    bool DocumentHasTrackType(TSharedPtr<FJsonObject> Document, const FString& ExpectedType)
    {
        const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
        if (!Document.IsValid() || !Document->TryGetArrayField(TEXT("animations"), Animations) || !Animations)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
        {
            const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
            if (!AnimationValue.IsValid() || !AnimationValue->TryGetObject(AnimationObj) || !AnimationObj || !AnimationObj->IsValid())
            {
                continue;
            }

            const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
            if (!(*AnimationObj)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
            {
                continue;
            }

            for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
            {
                const TSharedPtr<FJsonObject>* BindingObj = nullptr;
                if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !BindingObj->IsValid())
                {
                    continue;
                }

                const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
                if (!(*BindingObj)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
                {
                    continue;
                }

                for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
                {
                    const TSharedPtr<FJsonObject>* TrackObj = nullptr;
                    if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !TrackObj->IsValid())
                    {
                        continue;
                    }
                    FString TrackType;
                    (*TrackObj)->TryGetStringField(TEXT("type"), TrackType);
                    if (TrackType == ExpectedType)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    template<typename TrackType>
    TrackType* FindTrackByClass(UMovieScene* MovieScene)
    {
        if (!MovieScene)
        {
            return nullptr;
        }

        for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                if (TrackType* TypedTrack = Cast<TrackType>(Track))
                {
                    return TypedTrack;
                }
            }
        }
        return nullptr;
    }

    // Number of keys on the first float channel of the first float track of the named
    // animation, or -1 when any link in that chain is missing. Used to tell an animation
    // that was genuinely rebuilt from an imported document (1 key) apart from one the
    // importer left untouched (the 3-key fixture) — a distinction the animation count
    // alone cannot make.
    int32 CountFloatKeysOnAnimation(UWidgetBlueprint* WidgetBlueprint, const FName& AnimationName)
    {
        if (!WidgetBlueprint)
        {
            return -1;
        }
        for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
        {
            if (!Animation || Animation->GetFName() != AnimationName || !Animation->MovieScene)
            {
                continue;
            }
            UMovieSceneFloatTrack* FloatTrack = FindTrackByClass<UMovieSceneFloatTrack>(Animation->MovieScene);
            if (!FloatTrack || FloatTrack->GetAllSections().Num() == 0)
            {
                return -1;
            }
            UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
            return FloatSection ? FloatSection->GetChannel().GetTimes().Num() : -1;
        }
        return -1;
    }

    const FMovieSceneBinding* FindBindingForWidget(const UWidgetAnimation* Animation, const FName& WidgetName)
    {
        if (!Animation || !Animation->MovieScene)
        {
            return nullptr;
        }

        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.WidgetName == WidgetName)
            {
                return Animation->MovieScene->FindBinding(Binding.AnimationGuid);
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonFixtureCreatesFloatTrackTest,
    "PinWright.widget.animation_json.FixtureCreatesFloatTrack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonFixtureCreatesFloatTrackTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    const FWidgetAnimationFixture Fixture = CreateFloatAnimationFixture(TEXT("WBP_WidgetAnimationJsonFixture"));
    UWidgetTree* WidgetTree = Fixture.WidgetBlueprint ? Fixture.WidgetBlueprint->WidgetTree.Get() : nullptr;
    TestNotNull(TEXT("UWidgetBlueprint created"), Fixture.WidgetBlueprint);
    TestNotNull(TEXT("UWidgetTree created"), WidgetTree);
    TestNotNull(TEXT("named widget created"), Fixture.AnimatedWidget);
    TestNotNull(TEXT("animation created"), Fixture.Animation);
    TestNotNull(TEXT("movie scene created"), Fixture.MovieScene);
    TestNotNull(TEXT("UMovieSceneFloatTrack created"), Fixture.FloatTrack);
    TestNotNull(TEXT("float section created"), Fixture.FloatSection);
    TestTrue(TEXT("AnimationGuid is valid"), Fixture.BindingGuid.IsValid());

    if (!Fixture.WidgetBlueprint || !Fixture.Animation || !Fixture.MovieScene || !Fixture.FloatTrack || !Fixture.FloatSection)
    {
        return false;
    }

    TestEqual(TEXT("one UWidgetAnimation stored on blueprint"), Fixture.WidgetBlueprint->Animations.Num(), 1);
    TestEqual(TEXT("one widget animation binding stored"), Fixture.Animation->AnimationBindings.Num(), 1);
    TestEqual(TEXT("WidgetName preserved as binding identity"),
        Fixture.Animation->AnimationBindings[0].WidgetName,
        FName(TEXT("AnimatedLabel")));
    TestEqual(TEXT("AnimationGuid synchronized with MovieScene binding"),
        Fixture.Animation->AnimationBindings[0].AnimationGuid,
        Fixture.BindingGuid);

    const FMovieSceneBinding* MovieSceneBinding = Fixture.MovieScene->FindBinding(Fixture.BindingGuid);
    TestNotNull(TEXT("MovieScene binding exists"), MovieSceneBinding);
    if (MovieSceneBinding)
    {
        TestEqual(TEXT("MovieScene binding owns one track"), MovieSceneBinding->GetTracks().Num(), 1);
    }

    TestEqual(TEXT("property name set"),
        Fixture.FloatTrack->GetPropertyName(),
        FName(TEXT("RenderOpacity")));

    const FMovieSceneFloatChannel& Channel = Fixture.FloatSection->GetChannel();
    TestEqual(TEXT("FMovieSceneFloatChannel key count"), Channel.GetTimes().Num(), 3);
    if (Channel.GetValues().Num() == 3)
    {
        TestEqual(TEXT("first value is linear"), Channel.GetValues()[0].InterpMode.GetValue(), RCIM_Linear);
        TestEqual(TEXT("second value uses user tangent"), Channel.GetValues()[1].TangentMode.GetValue(), RCTM_User);
        TestEqual(TEXT("third value is constant"), Channel.GetValues()[2].InterpMode.GetValue(), RCIM_Constant);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonExportIncludesFloatKeysTest,
    "PinWright.widget.animation_json.ExportIncludesFloatKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonExportIncludesFloatKeysTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    const FWidgetAnimationFixture Fixture = CreateFloatAnimationFixture(TEXT("WBP_WidgetAnimationJsonExport"));
    TestNotNull(TEXT("fixture widget blueprint created"), Fixture.WidgetBlueprint);
    if (!Fixture.WidgetBlueprint)
    {
        return false;
    }

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("widget.export_animations_json"),
        MakePayloadWithWidgetPath(Fixture.WidgetPath),
        Capture);

    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("export succeeds"), Capture.bSuccess);
    TSharedPtr<FJsonObject> Document = GetExportedJsonDocument(Capture);
    TestTrue(TEXT("exported document exists"), Document.IsValid());
    if (!Document.IsValid())
    {
        return false;
    }

    FString Schema;
    Document->TryGetStringField(TEXT("schema"), Schema);
    TestEqual(TEXT("schema"), Schema, FString(TEXT("pinwright.widget-animations.v1")));

    const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
    TestTrue(TEXT("animations array exists"), Document->TryGetArrayField(TEXT("animations"), Animations));
    TestTrue(TEXT("one animation exported"), Animations && Animations->Num() == 1);
    if (!Animations || Animations->Num() != 1)
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
    (*Animations)[0]->TryGetObject(AnimationObj);
    TestTrue(TEXT("animation object exists"), AnimationObj && AnimationObj->IsValid());
    if (!AnimationObj || !AnimationObj->IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    (*AnimationObj)->TryGetArrayField(TEXT("bindings"), Bindings);
    TestTrue(TEXT("one binding exported"), Bindings && Bindings->Num() == 1);
    if (!Bindings || Bindings->Num() != 1)
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* BindingObj = nullptr;
    (*Bindings)[0]->TryGetObject(BindingObj);
    TestTrue(TEXT("binding object exists"), BindingObj && BindingObj->IsValid());
    if (!BindingObj || !BindingObj->IsValid())
    {
        return false;
    }
    FString WidgetName;
    (*BindingObj)->TryGetStringField(TEXT("widgetName"), WidgetName);
    TestEqual(TEXT("widget name identity exported"), WidgetName, FString(TEXT("AnimatedLabel")));

    const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
    (*BindingObj)->TryGetArrayField(TEXT("tracks"), Tracks);
    TestTrue(TEXT("one track exported"), Tracks && Tracks->Num() == 1);
    if (!Tracks || Tracks->Num() != 1)
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* TrackObj = nullptr;
    (*Tracks)[0]->TryGetObject(TrackObj);
    TestTrue(TEXT("track object exists"), TrackObj && TrackObj->IsValid());
    if (!TrackObj || !TrackObj->IsValid())
    {
        return false;
    }
    FString PropertyName;
    (*TrackObj)->TryGetStringField(TEXT("propertyName"), PropertyName);
    TestEqual(TEXT("propertyName exported"), PropertyName, FString(TEXT("RenderOpacity")));

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    (*TrackObj)->TryGetArrayField(TEXT("sections"), Sections);
    TestTrue(TEXT("one section exported"), Sections && Sections->Num() == 1);
    if (!Sections || Sections->Num() != 1)
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* SectionObj = nullptr;
    (*Sections)[0]->TryGetObject(SectionObj);
    TestTrue(TEXT("section object exists"), SectionObj && SectionObj->IsValid());
    if (!SectionObj || !SectionObj->IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
    (*SectionObj)->TryGetArrayField(TEXT("keys"), Keys);
    TestTrue(TEXT("three keys exported"), Keys && Keys->Num() == 3);
    if (Keys && Keys->Num() == 3)
    {
        const TSharedPtr<FJsonObject>* KeyObj = nullptr;
        TestTrue(TEXT("second key object exists"), (*Keys)[1]->TryGetObject(KeyObj) && KeyObj && KeyObj->IsValid());
        if (KeyObj && KeyObj->IsValid())
        {
            FString Interp;
            FString TangentMode;
            (*KeyObj)->TryGetStringField(TEXT("interp"), Interp);
            (*KeyObj)->TryGetStringField(TEXT("tangentMode"), TangentMode);
            TestEqual(TEXT("interp metadata exported"), Interp, FString(TEXT("cubic")));
            TestEqual(TEXT("tangent metadata exported"), TangentMode, FString(TEXT("user")));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonImportRoundTripTest,
    "PinWright.widget.animation_json.ImportRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonImportRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    const FWidgetAnimationFixture Source = CreateFloatAnimationFixture(TEXT("WBP_WidgetAnimationJsonSource"));
    UWidgetBlueprint* Target = CreateTransientWidgetBlueprint(MakeWidgetAnimationJsonTestAssetPath(TEXT("WBP_WidgetAnimationJsonTarget")));
    AddNamedTextBlock(Target, TEXT("AnimatedLabel"));

    FTestResponseCapture ExportCapture;
    InvokeHandlerWithCapture(
        TEXT("widget.export_animations_json"),
        MakePayloadWithWidgetPath(Source.WidgetPath),
        ExportCapture);
    TSharedPtr<FJsonObject> Document = GetExportedJsonDocument(ExportCapture);
    TestTrue(TEXT("export source document"), ExportCapture.bSuccess && Document.IsValid());
    if (!Target || !Document.IsValid())
    {
        return false;
    }

    TSharedPtr<FJsonObject> ImportPayload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    ImportPayload->SetObjectField(TEXT("json"), Document);
    ImportPayload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture ImportCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), ImportPayload, ImportCapture);
    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("import succeeds"), ImportCapture.bSuccess);
    TestEqual(TEXT("one imported animation"), Target->Animations.Num(), 1);
    if (Target->Animations.Num() != 1 || !Target->Animations[0] || !Target->Animations[0]->MovieScene)
    {
        return false;
    }

    UWidgetAnimation* ImportedAnimation = Target->Animations[0];
    TestEqual(TEXT("one imported binding"), ImportedAnimation->AnimationBindings.Num(), 1);
    const FGuid ImportedGuid = ImportedAnimation->AnimationBindings[0].AnimationGuid;
    TestTrue(TEXT("imported guid valid"), ImportedGuid.IsValid());
    TestNotNull(TEXT("MovieScene binding synchronized"), ImportedAnimation->MovieScene->FindBinding(ImportedGuid));

    const FMovieSceneBinding* Binding = ImportedAnimation->MovieScene->FindBinding(ImportedGuid);
    TestTrue(TEXT("binding owns track"), Binding && Binding->GetTracks().Num() == 1);
    UMovieSceneFloatTrack* FloatTrack = Binding ? Cast<UMovieSceneFloatTrack>(Binding->GetTracks()[0]) : nullptr;
    TestNotNull(TEXT("float track imported"), FloatTrack);
    if (!FloatTrack || FloatTrack->GetAllSections().Num() == 0)
    {
        return false;
    }

    UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    TestNotNull(TEXT("float section imported"), FloatSection);
    if (!FloatSection)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
    TestEqual(TEXT("key count round-tripped"), Channel.GetValues().Num(), 3);
    if (Channel.GetValues().Num() == 3)
    {
        TestEqual(TEXT("linear key preserved"), Channel.GetValues()[0].InterpMode.GetValue(), RCIM_Linear);
        TestEqual(TEXT("user tangent key preserved"), Channel.GetValues()[1].TangentMode.GetValue(), RCTM_User);
        TestEqual(TEXT("constant key preserved"), Channel.GetValues()[2].InterpMode.GetValue(), RCIM_Constant);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonRoundTripsCoreUmgTracksTest,
    "PinWright.widget.animation_json.RoundTripsCoreUmgTracks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonRoundTripsCoreUmgTracksTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    const FCoreUmgAnimationFixture Source = CreateCoreUmgAnimationFixture(TEXT("WBP_WidgetAnimationJsonCoreSource"));
    TestNotNull(TEXT("source widget blueprint created"), Source.WidgetBlueprint);
    TestNotNull(TEXT("source material track created"), Source.MaterialTrack);
    TestNotNull(TEXT("source transform track created"), Source.TransformTrack);
    TestNotNull(TEXT("source text track created"), Source.TextTrack);
    if (!Source.WidgetBlueprint || !Source.MaterialTrack || !Source.TransformTrack || !Source.TextTrack)
    {
        return false;
    }

    FTestResponseCapture ExportCapture;
    const bool bExportFound = InvokeHandlerWithCapture(
        TEXT("widget.export_animations_json"),
        MakePayloadWithWidgetPath(Source.WidgetPath),
        ExportCapture);

    TestTrue(TEXT("export handler found"), bExportFound);
    TestTrue(TEXT("export succeeds"), ExportCapture.bSuccess);
    double WarningCount = -1.0;
    TestTrue(TEXT("warningCount exported"), ExportCapture.Result.IsValid() && ExportCapture.Result->TryGetNumberField(TEXT("warningCount"), WarningCount));
    TestEqual(TEXT("export warningCount is zero"), static_cast<int32>(WarningCount), 0);

    TSharedPtr<FJsonObject> Document = GetExportedJsonDocument(ExportCapture);
    TestTrue(TEXT("exported document exists"), Document.IsValid());
    if (!Document.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("widgetMaterial track type exported"), DocumentHasTrackType(Document, TEXT("widgetMaterial")));
    TestTrue(TEXT("2dTransform track type exported"), DocumentHasTrackType(Document, TEXT("2dTransform")));
    TestTrue(TEXT("text track type exported"), DocumentHasTrackType(Document, TEXT("text")));

    UWidgetBlueprint* Target = CreateTransientWidgetBlueprint(MakeWidgetAnimationJsonTestAssetPath(TEXT("WBP_WidgetAnimationJsonCoreTarget")));
    AddNamedImage(Target, TEXT("AnimatedImage"));
    AddNamedTextBlock(Target, TEXT("AnimatedLabel"));
    TestNotNull(TEXT("target widget blueprint created"), Target);
    if (!Target)
    {
        return false;
    }

    TSharedPtr<FJsonObject> ImportPayload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    ImportPayload->SetObjectField(TEXT("json"), Document);
    ImportPayload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture ImportCapture;
    const bool bImportFound = InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), ImportPayload, ImportCapture);
    TestTrue(TEXT("import handler found"), bImportFound);
    TestTrue(TEXT("import succeeds"), ImportCapture.bSuccess);
    TestEqual(TEXT("one imported animation"), Target->Animations.Num(), 1);
    if (Target->Animations.Num() != 1 || !Target->Animations[0] || !Target->Animations[0]->MovieScene)
    {
        return false;
    }

    UMovieScene* ImportedMovieScene = Target->Animations[0]->MovieScene;
    UMovieSceneWidgetMaterialTrack* MaterialTrack = FindTrackByClass<UMovieSceneWidgetMaterialTrack>(ImportedMovieScene);
    UMovieScene2DTransformTrack* TransformTrack = FindTrackByClass<UMovieScene2DTransformTrack>(ImportedMovieScene);
    UMovieSceneTextTrack* TextTrack = FindTrackByClass<UMovieSceneTextTrack>(ImportedMovieScene);

    TestNotNull(TEXT("native widget material track imported"), MaterialTrack);
    TestNotNull(TEXT("native 2D transform track imported"), TransformTrack);
    TestNotNull(TEXT("native text track imported"), TextTrack);
    if (!MaterialTrack || !TransformTrack || !TextTrack)
    {
        return false;
    }

    const TArray<FName>& BrushPath = MaterialTrack->GetBrushPropertyNamePath();
    TestEqual(TEXT("material brush path count preserved"), BrushPath.Num(), 1);
    if (BrushPath.Num() == 1)
    {
        TestEqual(TEXT("material brush path value preserved"), BrushPath[0], FName(TEXT("Brush")));
    }

    UMovieSceneParameterSection* MaterialSection = MaterialTrack->GetAllSections().Num() > 0
        ? Cast<UMovieSceneParameterSection>(MaterialTrack->GetAllSections()[0])
        : nullptr;
    TestNotNull(TEXT("material parameter section imported"), MaterialSection);
    if (!MaterialSection)
    {
        return false;
    }

    TestEqual(TEXT("material scalar parameter count preserved"), MaterialSection->GetScalarParameterNamesAndCurves().Num(), 1);
    if (MaterialSection->GetScalarParameterNamesAndCurves().Num() == 1)
    {
        const FScalarParameterNameAndCurve& Scalar = MaterialSection->GetScalarParameterNamesAndCurves()[0];
        TestEqual(TEXT("material scalar parameter name preserved"), Scalar.ParameterName, FName(TEXT("FillAmount")));
        TestEqual(TEXT("material scalar key count preserved"), Scalar.ParameterCurve.GetValues().Num(), 2);
        if (Scalar.ParameterCurve.GetValues().Num() == 2)
        {
            TestEqual(TEXT("material scalar first key value preserved"), Scalar.ParameterCurve.GetValues()[0].Value, 0.2f);
            TestEqual(TEXT("material scalar second key value preserved"), Scalar.ParameterCurve.GetValues()[1].Value, 0.8f);
        }
    }

    TestEqual(TEXT("material color parameter count preserved"), MaterialSection->GetColorParameterNamesAndCurves().Num(), 1);
    if (MaterialSection->GetColorParameterNamesAndCurves().Num() == 1)
    {
        const FColorParameterNameAndCurves& Color = MaterialSection->GetColorParameterNamesAndCurves()[0];
        TestEqual(TEXT("material color parameter name preserved"), Color.ParameterName, FName(TEXT("Tint")));
        TestEqual(TEXT("material red key count preserved"), Color.RedCurve.GetValues().Num(), 1);
        TestEqual(TEXT("material green key count preserved"), Color.GreenCurve.GetValues().Num(), 1);
        TestEqual(TEXT("material blue key count preserved"), Color.BlueCurve.GetValues().Num(), 1);
        TestEqual(TEXT("material alpha key count preserved"), Color.AlphaCurve.GetValues().Num(), 1);
        if (Color.RedCurve.GetValues().Num() == 1 &&
            Color.GreenCurve.GetValues().Num() == 1 &&
            Color.BlueCurve.GetValues().Num() == 1 &&
            Color.AlphaCurve.GetValues().Num() == 1)
        {
            TestEqual(TEXT("material red key preserved"), Color.RedCurve.GetValues()[0].Value, 0.1f);
            TestEqual(TEXT("material green key preserved"), Color.GreenCurve.GetValues()[0].Value, 0.4f);
            TestEqual(TEXT("material blue key preserved"), Color.BlueCurve.GetValues()[0].Value, 0.7f);
            TestEqual(TEXT("material alpha key preserved"), Color.AlphaCurve.GetValues()[0].Value, 1.0f);
        }
    }

    const FMovieSceneBinding* TextBinding = FindBindingForWidget(Target->Animations[0], FName(TEXT("AnimatedLabel")));
    TestNotNull(TEXT("imported text widget binding exists"), TextBinding);
    if (!TextBinding)
    {
        return false;
    }

    UMovieScene2DTransformSection* TransformSection = TransformTrack->GetAllSections().Num() > 0
        ? Cast<UMovieScene2DTransformSection>(TransformTrack->GetAllSections()[0])
        : nullptr;
    TestNotNull(TEXT("2D transform section imported"), TransformSection);
    if (!TransformSection)
    {
        return false;
    }

    TestEqual(TEXT("translation X key count preserved"), TransformSection->Translation[0].GetValues().Num(), 1);
    TestEqual(TEXT("translation Y key count preserved"), TransformSection->Translation[1].GetValues().Num(), 1);
    TestEqual(TEXT("rotation key count preserved"), TransformSection->Rotation.GetValues().Num(), 1);
    TestEqual(TEXT("scale X key count preserved"), TransformSection->Scale[0].GetValues().Num(), 1);
    TestEqual(TEXT("scale Y key count preserved"), TransformSection->Scale[1].GetValues().Num(), 1);
    if (TransformSection->Translation[0].GetValues().Num() == 1 &&
        TransformSection->Translation[1].GetValues().Num() == 1 &&
        TransformSection->Rotation.GetValues().Num() == 1 &&
        TransformSection->Scale[0].GetValues().Num() == 1 &&
        TransformSection->Scale[1].GetValues().Num() == 1)
    {
        TestEqual(TEXT("translation X key value preserved"), TransformSection->Translation[0].GetValues()[0].Value, 12.0f);
        TestEqual(TEXT("translation Y key value preserved"), TransformSection->Translation[1].GetValues()[0].Value, 24.0f);
        TestEqual(TEXT("rotation key value preserved"), TransformSection->Rotation.GetValues()[0].Value, 45.0f);
        TestEqual(TEXT("scale X key value preserved"), TransformSection->Scale[0].GetValues()[0].Value, 1.5f);
        TestEqual(TEXT("scale Y key value preserved"), TransformSection->Scale[1].GetValues()[0].Value, 0.5f);
    }

    UMovieSceneTextSection* TextSection = TextTrack->GetAllSections().Num() > 0
        ? Cast<UMovieSceneTextSection>(TextTrack->GetAllSections()[0])
        : nullptr;
    TestNotNull(TEXT("text section imported"), TextSection);
    if (!TextSection)
    {
        return false;
    }

    TMovieSceneChannelData<const FText> TextData = TextSection->GetChannel().GetData();
    TestEqual(TEXT("text key count preserved"), TextData.GetValues().Num(), 2);
    if (TextData.GetValues().Num() == 2)
    {
        TestEqual(TEXT("first text key string preserved"), TextData.GetValues()[0].ToString(), FString(TEXT("Ready")));
        TestEqual(TEXT("second text key string preserved"), TextData.GetValues()[1].ToString(), FString(TEXT("Go")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonImportModeSemanticsTest,
    "PinWright.widget.animation_json.ImportModeSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonImportModeSemanticsTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    UWidgetBlueprint* Target = CreateTransientWidgetBlueprint(MakeWidgetAnimationJsonTestAssetPath(TEXT("WBP_WidgetAnimationJsonModes")));
    AddNamedTextBlock(Target, TEXT("AnimatedLabel"));
    FWidgetAnimationFixture Existing;
    AddFloatAnimationFixture(Target, TEXT("FadeIn"), TEXT("AnimatedLabel"), Existing);
    AddFloatAnimationFixture(Target, TEXT("Unrelated"), TEXT("AnimatedLabel"), Existing);
    TestEqual(TEXT("fixture starts with two animations"), Target ? Target->Animations.Num() : 0, 2);
    if (!Target)
    {
        return false;
    }

    // Both fixture animations start with the 3-key float channel AddFloatAnimationFixture
    // builds; the imported document carries a single key. The animation COUNT alone cannot
    // tell "same-name animation was rebuilt from the document" from "importer saw the name
    // collision and did nothing", because both leave two animations standing — so the key
    // counts are pinned before and after.
    TestEqual(TEXT("FadeIn starts with the 3-key fixture channel"),
        CountFloatKeysOnAnimation(Target, TEXT("FadeIn")), 3);
    TestEqual(TEXT("Unrelated starts with the 3-key fixture channel"),
        CountFloatKeysOnAnimation(Target, TEXT("Unrelated")), 3);

    TSharedPtr<FJsonObject> Document = MakeMinimalAnimationDocument(TEXT("AnimatedLabel"), TEXT("FadeIn"));
    TSharedPtr<FJsonObject> Payload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    Payload->SetObjectField(TEXT("json"), Document);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture ReplaceCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), Payload, ReplaceCapture);
    TestTrue(TEXT("replace import succeeds"), ReplaceCapture.bSuccess);
    TestEqual(TEXT("replace updates same-name animation and keeps unrelated animation"), Target->Animations.Num(), 2);
    TestEqual(TEXT("replace rebuilt FadeIn from the document's single key"),
        CountFloatKeysOnAnimation(Target, TEXT("FadeIn")), 1);
    TestEqual(TEXT("replace left the unrelated animation's keys untouched"),
        CountFloatKeysOnAnimation(Target, TEXT("Unrelated")), 3);

    Payload->SetStringField(TEXT("mode"), TEXT("merge"));
    FTestResponseCapture MergeCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), Payload, MergeCapture);
    TestTrue(TEXT("merge import succeeds"), MergeCapture.bSuccess);
    TestEqual(TEXT("merge updates same-name animation and keeps unrelated animation"), Target->Animations.Num(), 2);
    TestEqual(TEXT("merge rebuilt FadeIn from the document's single key"),
        CountFloatKeysOnAnimation(Target, TEXT("FadeIn")), 1);
    TestEqual(TEXT("merge left the unrelated animation's keys untouched"),
        CountFloatKeysOnAnimation(Target, TEXT("Unrelated")), 3);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonImportMinimalTimeValueKeyTest,
    "PinWright.widget.animation_json.ImportMinimalTimeValueKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonImportMinimalTimeValueKeyTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    UWidgetBlueprint* Target = CreateTransientWidgetBlueprint(MakeWidgetAnimationJsonTestAssetPath(TEXT("WBP_WidgetAnimationJsonMinimal")));
    AddNamedTextBlock(Target, TEXT("AnimatedLabel"));
    if (!Target)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    Payload->SetObjectField(TEXT("json"), MakeMinimalAnimationDocument(TEXT("AnimatedLabel"), TEXT("FadeIn")));
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), Payload, Capture);
    TestTrue(TEXT("minimal time/value key import succeeds"), Capture.bSuccess);
    if (Target->Animations.Num() != 1 || !Target->Animations[0] || !Target->Animations[0]->MovieScene)
    {
        return false;
    }

    const FGuid BindingGuid = Target->Animations[0]->AnimationBindings[0].AnimationGuid;
    const FMovieSceneBinding* Binding = Target->Animations[0]->MovieScene->FindBinding(BindingGuid);
    UMovieSceneFloatTrack* FloatTrack = Binding && Binding->GetTracks().Num() > 0
        ? Cast<UMovieSceneFloatTrack>(Binding->GetTracks()[0])
        : nullptr;
    UMovieSceneFloatSection* FloatSection = FloatTrack && FloatTrack->GetAllSections().Num() > 0
        ? Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0])
        : nullptr;
    TestNotNull(TEXT("minimal float section imported"), FloatSection);
    if (!FloatSection)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
    TestEqual(TEXT("one minimal key imported"), Channel.GetTimes().Num(), 1);
    if (Channel.GetTimes().Num() == 1 && Channel.GetValues().Num() == 1)
    {
        TestEqual(TEXT("time converted to frame using tick resolution"), Channel.GetTimes()[0], FFrameNumber(15));
        TestEqual(TEXT("minimal value imported"), Channel.GetValues()[0].Value, 0.25f);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonImportValidationTest,
    "PinWright.widget.animation_json.ImportValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonImportValidationTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    UWidgetBlueprint* Target = CreateTransientWidgetBlueprint(MakeWidgetAnimationJsonTestAssetPath(TEXT("WBP_WidgetAnimationJsonValidation")));
    AddNamedTextBlock(Target, TEXT("AnimatedLabel"));
    if (!Target)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    Payload->SetStringField(TEXT("mode"), TEXT("invalid"));
    Payload->SetObjectField(TEXT("json"), MakeShared<FJsonObject>());

    FTestResponseCapture InvalidModeCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), Payload, InvalidModeCapture);
    TestFalse(TEXT("invalid mode fails"), InvalidModeCapture.bSuccess);

    TSharedPtr<FJsonObject> MissingModePayload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    MissingModePayload->SetObjectField(TEXT("json"), MakeShared<FJsonObject>());
    FTestResponseCapture MissingModeCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), MissingModePayload, MissingModeCapture);
    TestFalse(TEXT("missing mode fails"), MissingModeCapture.bSuccess);

    TSharedPtr<FJsonObject> MalformedPayload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    MalformedPayload->SetStringField(TEXT("mode"), TEXT("replace"));
    MalformedPayload->SetStringField(TEXT("json"), TEXT("{"));
    FTestResponseCapture MalformedCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), MalformedPayload, MalformedCapture);
    TestFalse(TEXT("malformed JSON string fails"), MalformedCapture.bSuccess);

    TSharedPtr<FJsonObject> MissingTargetPayload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    MissingTargetPayload->SetStringField(TEXT("mode"), TEXT("replace"));
    MissingTargetPayload->SetObjectField(TEXT("json"), MakeMinimalAnimationDocument(TEXT("MissingLabel"), TEXT("FadeIn")));
    FTestResponseCapture MissingTargetCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), MissingTargetPayload, MissingTargetCapture);
    TestFalse(TEXT("missing target widget fails"), MissingTargetCapture.bSuccess);
    TestEqual(TEXT("missing target validation does not mutate animations"), Target->Animations.Num(), 0);

    TSharedPtr<FJsonObject> UnsupportedShapeDocument = MakeMinimalAnimationDocument(TEXT("AnimatedLabel"), TEXT("FadeIn"));
    TSharedPtr<FJsonObject> BindingObj = GetFirstBinding(UnsupportedShapeDocument);
    if (BindingObj.IsValid())
    {
        BindingObj->SetStringField(TEXT("slotWidgetName"), TEXT("UnsupportedSlot"));
    }
    TSharedPtr<FJsonObject> UnsupportedShapePayload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    UnsupportedShapePayload->SetStringField(TEXT("mode"), TEXT("replace"));
    UnsupportedShapePayload->SetObjectField(TEXT("json"), UnsupportedShapeDocument);
    FTestResponseCapture UnsupportedShapeCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), UnsupportedShapePayload, UnsupportedShapeCapture);
    TestFalse(TEXT("unsupported binding shape fails"), UnsupportedShapeCapture.bSuccess);
    TestEqual(TEXT("unsupported binding shape validation does not mutate animations"), Target->Animations.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonExportEmitsNoCommonPropertyWarningsTest,
    "PinWright.widget.animation_json.ExportEmitsNoCommonPropertyWarnings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonExportEmitsNoCommonPropertyWarningsTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    const FCommonPropertyAnimationFixture Source = CreateCommonPropertyAnimationFixture(TEXT("WBP_CommonPropNoWarnSource"));
    TestNotNull(TEXT("source widget blueprint created"), Source.WidgetBlueprint);
    TestNotNull(TEXT("color track created"), Source.ColorTrack);
    TestNotNull(TEXT("byte track created"), Source.ByteTrack);
    TestNotNull(TEXT("floatVector track created"), Source.FloatVectorTrack);
    TestNotNull(TEXT("doubleVector track created"), Source.DoubleVectorTrack);
    if (!Source.WidgetBlueprint || !Source.ColorTrack || !Source.ByteTrack || !Source.FloatVectorTrack || !Source.DoubleVectorTrack)
    {
        return false;
    }

    FTestResponseCapture ExportCapture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("widget.export_animations_json"),
        MakePayloadWithWidgetPath(Source.WidgetPath),
        ExportCapture);

    TestTrue(TEXT("export handler found"), bFound);
    TestTrue(TEXT("export succeeds"), ExportCapture.bSuccess);

    double WarningCount = -1.0;
    const bool bHasWarningCount = ExportCapture.Result.IsValid() && ExportCapture.Result->TryGetNumberField(TEXT("warningCount"), WarningCount);
    TestTrue(TEXT("warningCount field present"), bHasWarningCount);
    TestEqual(TEXT("export warningCount is zero for common property tracks"), static_cast<int32>(WarningCount), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonRoundTripsCommonPropertyTracksTest,
    "PinWright.widget.animation_json.RoundTripsCommonPropertyTracks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonRoundTripsCommonPropertyTracksTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    const FCommonPropertyAnimationFixture Source = CreateCommonPropertyAnimationFixture(TEXT("WBP_CommonPropRoundTripSource"));
    TestNotNull(TEXT("source widget blueprint created"), Source.WidgetBlueprint);
    TestNotNull(TEXT("source color track created"), Source.ColorTrack);
    TestNotNull(TEXT("source byte track created"), Source.ByteTrack);
    TestNotNull(TEXT("source floatVector track created"), Source.FloatVectorTrack);
    TestNotNull(TEXT("source doubleVector track created"), Source.DoubleVectorTrack);
    if (!Source.WidgetBlueprint || !Source.ColorTrack || !Source.ByteTrack || !Source.FloatVectorTrack || !Source.DoubleVectorTrack)
    {
        return false;
    }

    FTestResponseCapture ExportCapture;
    const bool bExportFound = InvokeHandlerWithCapture(
        TEXT("widget.export_animations_json"),
        MakePayloadWithWidgetPath(Source.WidgetPath),
        ExportCapture);

    TestTrue(TEXT("export handler found"), bExportFound);
    TestTrue(TEXT("export succeeds"), ExportCapture.bSuccess);

    double WarningCount = -1.0;
    TestTrue(TEXT("warningCount exported"), ExportCapture.Result.IsValid() && ExportCapture.Result->TryGetNumberField(TEXT("warningCount"), WarningCount));
    TestEqual(TEXT("export produces no warnings"), static_cast<int32>(WarningCount), 0);

    TSharedPtr<FJsonObject> Document = GetExportedJsonDocument(ExportCapture);
    TestTrue(TEXT("exported document exists"), Document.IsValid());
    if (!Document.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("color track type exported"), DocumentHasTrackType(Document, TEXT("color")));
    TestTrue(TEXT("byte track type exported"), DocumentHasTrackType(Document, TEXT("byte")));
    TestTrue(TEXT("floatVector track type exported"), DocumentHasTrackType(Document, TEXT("floatVector")));
    TestTrue(TEXT("doubleVector track type exported"), DocumentHasTrackType(Document, TEXT("doubleVector")));

    UWidgetBlueprint* Target = CreateTransientWidgetBlueprint(MakeWidgetAnimationJsonTestAssetPath(TEXT("WBP_CommonPropRoundTripTarget")));
    AddNamedImage(Target, TEXT("AnimatedImage"));
    TestNotNull(TEXT("target widget blueprint created"), Target);
    if (!Target)
    {
        return false;
    }

    TSharedPtr<FJsonObject> ImportPayload = MakePayloadWithWidgetPath(Target->GetOutermost()->GetName());
    ImportPayload->SetObjectField(TEXT("json"), Document);
    ImportPayload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture ImportCapture;
    const bool bImportFound = InvokeHandlerWithCapture(TEXT("widget.import_animations_json"), ImportPayload, ImportCapture);
    TestTrue(TEXT("import handler found"), bImportFound);
    TestTrue(TEXT("import succeeds"), ImportCapture.bSuccess);
    TestEqual(TEXT("one imported animation"), Target->Animations.Num(), 1);
    if (Target->Animations.Num() != 1 || !Target->Animations[0] || !Target->Animations[0]->MovieScene)
    {
        return false;
    }

    UMovieScene* ImportedMovieScene = Target->Animations[0]->MovieScene;
    UMovieSceneColorTrack* ColorTrack = FindTrackByClass<UMovieSceneColorTrack>(ImportedMovieScene);
    UMovieSceneByteTrack* ByteTrack = FindTrackByClass<UMovieSceneByteTrack>(ImportedMovieScene);
    UMovieSceneFloatVectorTrack* FloatVectorTrack = FindTrackByClass<UMovieSceneFloatVectorTrack>(ImportedMovieScene);
    UMovieSceneDoubleVectorTrack* DoubleVectorTrack = FindTrackByClass<UMovieSceneDoubleVectorTrack>(ImportedMovieScene);

    TestNotNull(TEXT("color track imported"), ColorTrack);
    TestNotNull(TEXT("byte track imported"), ByteTrack);
    TestNotNull(TEXT("floatVector track imported"), FloatVectorTrack);
    TestNotNull(TEXT("doubleVector track imported"), DoubleVectorTrack);

    // Color track assertions
    if (ColorTrack && ColorTrack->GetAllSections().Num() > 0)
    {
        UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(ColorTrack->GetAllSections()[0]);
        TestNotNull(TEXT("color section imported"), ColorSection);
        if (ColorSection)
        {
            TestEqual(TEXT("color red key count preserved"), ColorSection->GetRedChannel().GetTimes().Num(), 2);
            TestEqual(TEXT("color green key count preserved"), ColorSection->GetGreenChannel().GetTimes().Num(), 1);
            TestEqual(TEXT("color alpha key count preserved"), ColorSection->GetAlphaChannel().GetTimes().Num(), 1);
            if (ColorSection->GetRedChannel().GetValues().Num() == 2)
            {
                TestEqual(TEXT("color red first key value preserved"), ColorSection->GetRedChannel().GetValues()[0].Value, 0.0f);
                TestEqual(TEXT("color red second key value preserved"), ColorSection->GetRedChannel().GetValues()[1].Value, 1.0f);
            }
        }
    }

    // Byte track assertions
    if (ByteTrack)
    {
        TestEqual(TEXT("byte track enum preserved"), ByteTrack->GetEnum(), StaticEnum<ESlateVisibility>());
        if (ByteTrack->GetAllSections().Num() > 0)
        {
            UMovieSceneByteSection* ByteSection = Cast<UMovieSceneByteSection>(ByteTrack->GetAllSections()[0]);
            TestNotNull(TEXT("byte section imported"), ByteSection);
            if (ByteSection)
            {
                TestEqual(TEXT("byte key count preserved"), ByteSection->ByteCurve.GetTimes().Num(), 1);
                if (ByteSection->ByteCurve.GetValues().Num() == 1)
                {
                    TestEqual(TEXT("byte key value preserved (ESlateVisibility::Hidden)"),
                        ByteSection->ByteCurve.GetValues()[0],
                        static_cast<uint8>(ESlateVisibility::Hidden));
                }
            }
        }
    }

    // FloatVector track assertions
    if (FloatVectorTrack)
    {
        TestEqual(TEXT("floatVector channelsUsed preserved"), FloatVectorTrack->GetNumChannelsUsed(), 3);
        if (FloatVectorTrack->GetAllSections().Num() > 0)
        {
            UMovieSceneFloatVectorSection* VectorSection = Cast<UMovieSceneFloatVectorSection>(FloatVectorTrack->GetAllSections()[0]);
            TestNotNull(TEXT("floatVector section imported"), VectorSection);
            if (VectorSection)
            {
                TestEqual(TEXT("floatVector channelsUsed on section preserved"), VectorSection->GetChannelsUsed(), 3);
                TestEqual(TEXT("floatVector x key count preserved"), VectorSection->GetChannel(0).GetTimes().Num(), 1);
                TestEqual(TEXT("floatVector y key count preserved"), VectorSection->GetChannel(1).GetTimes().Num(), 1);
                TestEqual(TEXT("floatVector z key count preserved"), VectorSection->GetChannel(2).GetTimes().Num(), 1);
                if (VectorSection->GetChannel(0).GetValues().Num() == 1)
                {
                    TestEqual(TEXT("floatVector x first key value preserved"), VectorSection->GetChannel(0).GetValues()[0].Value, 1.0f);
                }
                if (VectorSection->GetChannel(1).GetValues().Num() == 1)
                {
                    TestEqual(TEXT("floatVector y first key value preserved"), VectorSection->GetChannel(1).GetValues()[0].Value, 2.0f);
                }
                if (VectorSection->GetChannel(2).GetValues().Num() == 1)
                {
                    TestEqual(TEXT("floatVector z first key value preserved"), VectorSection->GetChannel(2).GetValues()[0].Value, 3.0f);
                }
            }
        }
    }

    // DoubleVector track assertions
    if (DoubleVectorTrack)
    {
        TestEqual(TEXT("doubleVector channelsUsed preserved"), DoubleVectorTrack->GetNumChannelsUsed(), 2);
        if (DoubleVectorTrack->GetAllSections().Num() > 0)
        {
            UMovieSceneDoubleVectorSection* VectorSection = Cast<UMovieSceneDoubleVectorSection>(DoubleVectorTrack->GetAllSections()[0]);
            TestNotNull(TEXT("doubleVector section imported"), VectorSection);
            if (VectorSection)
            {
                TestEqual(TEXT("doubleVector channelsUsed on section preserved"), VectorSection->GetChannelsUsed(), 2);
                TestEqual(TEXT("doubleVector x key count preserved"), VectorSection->GetChannel(0).GetTimes().Num(), 1);
                TestEqual(TEXT("doubleVector y key count preserved"), VectorSection->GetChannel(1).GetTimes().Num(), 1);
                if (VectorSection->GetChannel(0).GetValues().Num() == 1)
                {
                    TestTrue(TEXT("doubleVector x first key value preserved near 10.5"),
                        FMath::Abs(VectorSection->GetChannel(0).GetValues()[0].Value - 10.5) < 0.001);
                }
                if (VectorSection->GetChannel(1).GetValues().Num() == 1)
                {
                    TestTrue(TEXT("doubleVector y first key value preserved near 20.25"),
                        FMath::Abs(VectorSection->GetChannel(1).GetValues()[0].Value - 20.25) < 0.001);
                }
            }
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationJsonExportRootAndSlotBindingsEmittedTest,
    "PinWright.widget.export_animations_json.RootAndSlotBindingsEmitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationJsonExportRootAndSlotBindingsEmittedTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    FWidgetAnimationFixture Fixture = CreateFloatAnimationFixture(TEXT("WBP_WidgetAnimationJsonRootSlot"));
    TestNotNull(TEXT("fixture widget blueprint created"), Fixture.WidgetBlueprint);
    TestNotNull(TEXT("fixture root widget present"), Fixture.RootWidget);
    TestNotNull(TEXT("fixture animation created"), Fixture.Animation);
    TestNotNull(TEXT("fixture movie scene created"), Fixture.MovieScene);
    if (!Fixture.WidgetBlueprint || !Fixture.RootWidget || !Fixture.Animation || !Fixture.MovieScene)
    {
        return false;
    }

    // Add root-widget binding: WidgetName == root canvas FName, bIsRootWidget=true.
    const FName RootName = Fixture.RootWidget->GetFName();
    const FGuid RootGuid = Fixture.MovieScene->AddPossessable(RootName.ToString(), Fixture.RootWidget->GetClass());
    TestTrue(TEXT("root possessable guid valid"), RootGuid.IsValid());
    if (RootGuid.IsValid())
    {
        FWidgetAnimationBinding RootBinding;
        RootBinding.WidgetName = RootName;
        RootBinding.AnimationGuid = RootGuid;
        RootBinding.SlotWidgetName = NAME_None;
        RootBinding.bIsRootWidget = true;
        Fixture.Animation->AnimationBindings.Add(RootBinding);

        UMovieSceneFloatTrack* RootTrack = Fixture.MovieScene->AddTrack<UMovieSceneFloatTrack>(RootGuid);
        if (RootTrack)
        {
            RootTrack->SetPropertyNameAndPath(FName(TEXT("RenderOpacity")), TEXT("RenderOpacity"));
            UMovieSceneFloatSection* RootSection = Cast<UMovieSceneFloatSection>(RootTrack->CreateNewSection());
            if (RootSection)
            {
                RootSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                RootSection->GetChannel().AddLinearKey(FFrameNumber(0), 0.0f);
                RootTrack->AddSection(*RootSection);
            }
        }
    }

    // Add slot-widget binding: SlotWidgetName="CanvasPanelSlot".
    const FName SlotHostName(TEXT("AnimatedLabel"));
    const FGuid SlotGuid = Fixture.MovieScene->AddPossessable(SlotHostName.ToString(), UObject::StaticClass());
    TestTrue(TEXT("slot possessable guid valid"), SlotGuid.IsValid());
    if (SlotGuid.IsValid())
    {
        FWidgetAnimationBinding SlotBinding;
        SlotBinding.WidgetName = SlotHostName;
        SlotBinding.AnimationGuid = SlotGuid;
        SlotBinding.SlotWidgetName = FName(TEXT("CanvasPanelSlot"));
        SlotBinding.bIsRootWidget = false;
        Fixture.Animation->AnimationBindings.Add(SlotBinding);

        UMovieSceneFloatTrack* SlotTrack = Fixture.MovieScene->AddTrack<UMovieSceneFloatTrack>(SlotGuid);
        if (SlotTrack)
        {
            SlotTrack->SetPropertyNameAndPath(FName(TEXT("Padding")), TEXT("Padding"));
            UMovieSceneFloatSection* SlotSection = Cast<UMovieSceneFloatSection>(SlotTrack->CreateNewSection());
            if (SlotSection)
            {
                SlotSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                SlotSection->GetChannel().AddLinearKey(FFrameNumber(0), 0.0f);
                SlotTrack->AddSection(*SlotSection);
            }
        }
    }

    // Invoke production code path directly.
    TSharedPtr<FJsonObject> Document;
    TArray<FString> Warnings;
    FString Error;
    const bool bExported = WidgetAnimationJson::ExportAnimations(
        Fixture.WidgetBlueprint, Document, Warnings, Error, /*bIncludeEventMetadata=*/false);
    TestTrue(TEXT("ExportAnimations succeeds"), bExported);
    TestTrue(TEXT("export document exists"), Document.IsValid());
    if (!bExported || !Document.IsValid())
    {
        return false;
    }

    // Locate animation by name in the document.
    const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
    TestTrue(TEXT("animations array exists"), Document->TryGetArrayField(TEXT("animations"), Animations));
    if (!Animations)
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
    for (const TSharedPtr<FJsonValue>& Value : *Animations)
    {
        const TSharedPtr<FJsonObject>* CandidateObj = nullptr;
        if (Value.IsValid() && Value->TryGetObject(CandidateObj) && CandidateObj && CandidateObj->IsValid())
        {
            FString Name;
            (*CandidateObj)->TryGetStringField(TEXT("name"), Name);
            if (Name == Fixture.Animation->GetName())
            {
                AnimationObj = CandidateObj;
                break;
            }
        }
    }
    TestTrue(TEXT("fixture animation present in document"), AnimationObj && AnimationObj->IsValid());
    if (!AnimationObj || !AnimationObj->IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    TestTrue(TEXT("bindings array exists"), (*AnimationObj)->TryGetArrayField(TEXT("bindings"), Bindings));
    if (!Bindings)
    {
        return false;
    }
    TestEqual(TEXT("three bindings emitted (normal + root + slot)"), Bindings->Num(), 3);

    bool bFoundRoot = false;
    bool bFoundSlot = false;
    bool bFoundNormal = false;
    for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
    {
        const TSharedPtr<FJsonObject>* BindingObj = nullptr;
        if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !BindingObj->IsValid())
        {
            continue;
        }
        FString WidgetName;
        (*BindingObj)->TryGetStringField(TEXT("widgetName"), WidgetName);

        bool bIsRootField = false;
        const bool bHasIsRoot = (*BindingObj)->TryGetBoolField(TEXT("isRootWidget"), bIsRootField);
        FString SlotName;
        const bool bHasSlot = (*BindingObj)->TryGetStringField(TEXT("slotWidgetName"), SlotName);

        if (WidgetName == RootName.ToString())
        {
            bFoundRoot = true;
            TestTrue(TEXT("root binding has isRootWidget=true"), bHasIsRoot && bIsRootField);
            TestFalse(TEXT("root binding has no slotWidgetName"), bHasSlot);
        }
        else if (bHasSlot && SlotName == TEXT("CanvasPanelSlot"))
        {
            bFoundSlot = true;
            // isRootWidget absent or false.
            bool bSlotIsRoot = false;
            const bool bSlotHasIsRoot = (*BindingObj)->TryGetBoolField(TEXT("isRootWidget"), bSlotIsRoot);
            TestTrue(TEXT("slot binding isRootWidget absent or false"), !bSlotHasIsRoot || !bSlotIsRoot);
            const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
            TestTrue(TEXT("slot binding tracks array exists"), (*BindingObj)->TryGetArrayField(TEXT("tracks"), SlotTracks));
            if (SlotTracks)
            {
                TestEqual(TEXT("slot binding has one track"), SlotTracks->Num(), 1);
            }
        }
        else if (WidgetName == TEXT("AnimatedLabel"))
        {
            bFoundNormal = true;
            TestFalse(TEXT("normal binding has no isRootWidget field"), bHasIsRoot);
            TestFalse(TEXT("normal binding has no slotWidgetName field"), bHasSlot);
        }
    }
    TestTrue(TEXT("root binding emitted"), bFoundRoot);
    TestTrue(TEXT("slot binding emitted"), bFoundSlot);
    TestTrue(TEXT("normal binding emitted"), bFoundNormal);

    for (const FString& Warning : Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("warning does not mention v1 binding shape: %s"), *Warning),
            Warning.Contains(TEXT("unsupported v1 binding shape")));
    }

    return true;
}
