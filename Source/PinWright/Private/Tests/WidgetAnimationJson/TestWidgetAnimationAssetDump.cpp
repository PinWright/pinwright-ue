// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"


#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Assets/AssetDumpTestHelpers.h"
#include "Tests/Utility/AssetDumpFixtureHelpers.h"
#include "WidgetAnimationJsonTestUtils.h"
#include "Channels/MovieSceneEvent.h"
#include "MovieScene.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "WidgetBlueprint.h"

namespace
{
    using AssetDumpFixtureHelpers::LoadJsonFile;
    using AssetDumpTestHelpers::FindDumpFile;

    TSharedPtr<FJsonObject> FindAnimationByName(const TSharedPtr<FJsonObject>& Document, const FString& AnimationName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
        if (!Document.IsValid() || !Document->TryGetArrayField(TEXT("animations"), Animations) || !Animations)
        {
            return nullptr;
        }

        for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
        {
            const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
            if (!AnimationValue.IsValid()
                || !AnimationValue->TryGetObject(AnimationObj)
                || !AnimationObj
                || !AnimationObj->IsValid())
            {
                continue;
            }

            FString Name;
            (*AnimationObj)->TryGetStringField(TEXT("name"), Name);
            if (Name == AnimationName)
            {
                return *AnimationObj;
            }
        }
        return nullptr;
    }

    bool HasBindingWithTracks(const TSharedPtr<FJsonObject>& Animation)
    {
        const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
        if (!Animation.IsValid() || !Animation->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings || Bindings->Num() == 0)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
        {
            const TSharedPtr<FJsonObject>* BindingObj = nullptr;
            if (!BindingValue.IsValid()
                || !BindingValue->TryGetObject(BindingObj)
                || !BindingObj
                || !BindingObj->IsValid())
            {
                continue;
            }

            const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
            if ((*BindingObj)->TryGetArrayField(TEXT("tracks"), Tracks) && Tracks && Tracks->Num() > 0)
            {
                return true;
            }
        }
        return false;
    }

    // Adds a sequence-level event track (UMovieSceneEventTrack) with one keyed
    // event so the dumped animation carries event metadata (eventTrackCount > 0,
    // triggeredFunctionCount > 0 via the compiled function name). Mirrors the
    // helper in WidgetAnimationEventIntrospectionTests.cpp.
    bool AddSequenceEventTrackForDumpFixture(UMovieScene* MovieScene)
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

    // The predecessor of this helper returned a single bool whose payload condition was
    // (eventTrackCount > 0 || delegateBindingCount > 0 || triggeredFunctionCount > 0) —
    // a disjunction that stayed true when the dump dropped two of the three counters.
    // AddSequenceEventTrackForDumpFixture plants exactly one UMovieSceneEventTrack with one
    // key whose CompiledFunctionName is "SequenceEvent", so every counter is knowable and is
    // pinned individually here, including the negative half (no delegate binding is planted,
    // so delegateBindingCount must be exactly 0 — a dump that invents one is now caught).
    void ExpectEventMetadata(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Animation)
    {
        const TArray<TSharedPtr<FJsonValue>>* EventTracks = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* DelegateBindings = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* TriggeredFunctions = nullptr;
        Test.TestTrue(TEXT("eventTracks array emitted"),
            Animation.IsValid() && Animation->TryGetArrayField(TEXT("eventTracks"), EventTracks));
        Test.TestTrue(TEXT("delegateBindings array emitted"),
            Animation.IsValid() && Animation->TryGetArrayField(TEXT("delegateBindings"), DelegateBindings));
        Test.TestTrue(TEXT("triggeredFunctions array emitted"),
            Animation.IsValid() && Animation->TryGetArrayField(TEXT("triggeredFunctions"), TriggeredFunctions));

        double EventTrackCount = -1.0;
        double DelegateBindingCount = -1.0;
        double TriggeredFunctionCount = -1.0;
        if (Animation.IsValid())
        {
            Animation->TryGetNumberField(TEXT("eventTrackCount"), EventTrackCount);
            Animation->TryGetNumberField(TEXT("delegateBindingCount"), DelegateBindingCount);
            Animation->TryGetNumberField(TEXT("triggeredFunctionCount"), TriggeredFunctionCount);
        }

        Test.TestEqual(TEXT("eventTrackCount is the one planted sequence event track"),
            static_cast<int32>(EventTrackCount), 1);
        Test.TestEqual(TEXT("triggeredFunctionCount is the one compiled event function"),
            static_cast<int32>(TriggeredFunctionCount), 1);
        Test.TestEqual(TEXT("delegateBindingCount is zero — no delegate binding was planted"),
            static_cast<int32>(DelegateBindingCount), 0);

        if (EventTracks && EventTracks->Num() == 1)
        {
            Test.TestEqual(TEXT("eventTracks array length matches eventTrackCount"),
                EventTracks->Num(), static_cast<int32>(EventTrackCount));
        }
        if (TriggeredFunctions && TriggeredFunctions->Num() == 1)
        {
            FString FunctionName;
            (*TriggeredFunctions)[0]->TryGetString(FunctionName);
            Test.TestEqual(TEXT("triggeredFunctions names the planted CompiledFunctionName"),
                FunctionName, FString(TEXT("SequenceEvent")));
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetAnimationAspectTrackStartTimerTest,
    "PinWright.Assets.AssetDump.WidgetAnimationAspect.TrackStartTimer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWidgetAnimationAspectTrackStartTimerTest::RunTest(const FString& Parameters)
{
    using namespace WidgetAnimationJsonTestUtils;

    // Build the fixture in-process instead of depending on a dev-project-only
    // asset (formerly a host-project HUD timer widget, absent on a clean CI
    // host). The transient WidgetBlueprint carries a "Full" animation with a
    // float track (bindings-with-tracks) plus a sequence event track (event
    // metadata) — exactly what the assertions below verify. DumpSingleAsset
    // resolves in-memory packages, so no save-to-disk is needed.
    const FString WidgetPackagePath = MakeWidgetAnimationJsonTestAssetPath(TEXT("W_TrackStartTimerFixture"));
    UWidgetBlueprint* WidgetBlueprint = CreateTransientWidgetBlueprint(WidgetPackagePath);
    TestNotNull(TEXT("Transient widget blueprint created"), WidgetBlueprint);
    if (!WidgetBlueprint)
    {
        return false;
    }

    FWidgetAnimationFixture Fixture;
    const bool bFixtureBuilt = AddFloatAnimationFixture(
        WidgetBlueprint, TEXT("Full"), TEXT("AnimatedLabel"), Fixture);
    TestTrue(TEXT("Full animation fixture built"), bFixtureBuilt);
    if (!bFixtureBuilt)
    {
        return false;
    }

    const bool bEventTrackAdded = AddSequenceEventTrackForDumpFixture(Fixture.MovieScene);
    TestTrue(TEXT("Sequence event track added to Full animation"), bEventTrackAdded);
    if (!bEventTrackAdded)
    {
        return false;
    }

    const FString AssetPath = WidgetBlueprint->GetPathName();
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("WidgetAnimationAssetDumpTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(AssetPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient TrackStartTimer fixture"), Result.ErrorCode.IsEmpty());
    if (!Result.ErrorCode.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return false;
    }

    const FString WidgetAnimationsPath = FindDumpFile(Result.WrittenPaths, DumpFileNames::WidgetAnimations);
    TestFalse(TEXT("widget_animations.json path is present"), WidgetAnimationsPath.IsEmpty());
    if (WidgetAnimationsPath.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return false;
    }

    const TSharedPtr<FJsonObject> Document = LoadJsonFile(WidgetAnimationsPath);
    TestTrue(TEXT("widget_animations.json parses"), Document.IsValid());
    if (!Document.IsValid())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return false;
    }

    FString Schema;
    Document->TryGetStringField(TEXT("schema"), Schema);
    TestEqual(TEXT("schema"), Schema, FString(TEXT("pinwright.widget-animations.v1")));

    const TSharedPtr<FJsonObject> FullAnimation = FindAnimationByName(Document, TEXT("Full"));
    TestTrue(TEXT("Full animation exists"), FullAnimation.IsValid());
    if (!FullAnimation.IsValid())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return false;
    }

    TestTrue(TEXT("Full animation has bindings with tracks"), HasBindingWithTracks(FullAnimation));
    ExpectEventMetadata(*this, FullAnimation);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}
