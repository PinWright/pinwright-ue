// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for widget animation requests that must validate completely before
// repairing or extending the serialized MovieScene.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Handlers/UI/WidgetAnimationTestHooks.h"
#endif

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace
{
    UWidgetAnimation* AddAnimationFixture(
        UWidgetBlueprint* WidgetBP,
        const FName& AnimationName,
        const FName& MovieSceneName = NAME_None)
    {
        if (!WidgetBP)
        {
            return nullptr;
        }

        UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(
            WidgetBP, AnimationName, RF_Transactional);
        if (!Animation)
        {
            return nullptr;
        }

        if (!MovieSceneName.IsNone())
        {
            Animation->MovieScene = NewObject<UMovieScene>(
                Animation, MovieSceneName, RF_Transactional);
            if (!Animation->MovieScene)
            {
                return nullptr;
            }
            Animation->MovieScene->SetDisplayRate(FFrameRate(30, 1));
            Animation->MovieScene->SetPlaybackRange(
                TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
        }

        WidgetBP->Animations.Add(Animation);
        WidgetTestFixtures::RegisterWidgetVariable(WidgetBP, AnimationName);
        return Animation;
    }

    struct FAnimationSnapshot
    {
        UMovieScene* MovieScene = nullptr;
        FName MovieSceneName = NAME_None;
        TRange<FFrameNumber> PlaybackRange;
        UObject* DesiredNameOccupant = nullptr;
        UObject* DesiredNameOccupantOuter = nullptr;
        FName DesiredNameOccupantName = NAME_None;
        bool bPackageDirty = false;
        int32 PossessableCount = 0;
        int32 MovieSceneBindingCount = 0;
        int32 TrackCount = 0;
        int32 SectionCount = 0;
        TArray<int32> FloatKeyCounts;
        int32 AnimationBindingCount = 0;
    };

    FAnimationSnapshot SnapshotAnimation(const UWidgetAnimation* Animation)
    {
        FAnimationSnapshot Snapshot;
        if (!Animation)
        {
            return Snapshot;
        }

        Snapshot.MovieScene = Animation->GetMovieScene();
        Snapshot.AnimationBindingCount = Animation->AnimationBindings.Num();
        Snapshot.bPackageDirty = Animation->GetOutermost()->IsDirty();
        Snapshot.DesiredNameOccupant = StaticFindObjectFastSafe(
            UObject::StaticClass(),
            const_cast<UWidgetAnimation*>(Animation),
            Animation->GetFName());
        if (Snapshot.DesiredNameOccupant)
        {
            Snapshot.DesiredNameOccupantOuter = Snapshot.DesiredNameOccupant->GetOuter();
            Snapshot.DesiredNameOccupantName = Snapshot.DesiredNameOccupant->GetFName();
        }
        if (!Snapshot.MovieScene)
        {
            return Snapshot;
        }

        Snapshot.MovieSceneName = Snapshot.MovieScene->GetFName();
        Snapshot.PlaybackRange = Snapshot.MovieScene->GetPlaybackRange();
        const TArray<FMovieSceneBinding>& Bindings =
            static_cast<const UMovieScene*>(Snapshot.MovieScene)->GetBindings();
        Snapshot.PossessableCount = Snapshot.MovieScene->GetPossessableCount();
        Snapshot.MovieSceneBindingCount = Bindings.Num();
        auto CaptureTrack = [&Snapshot](const UMovieSceneTrack* Track)
        {
            if (!Track)
            {
                return;
            }

            ++Snapshot.TrackCount;
            const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
            Snapshot.SectionCount += Sections.Num();
            for (const UMovieSceneSection* Section : Sections)
            {
                const UMovieSceneFloatSection* FloatSection =
                    Cast<UMovieSceneFloatSection>(Section);
                if (FloatSection)
                {
                    Snapshot.FloatKeyCounts.Add(FloatSection->GetChannel().GetNumKeys());
                }
            }
        };
        for (const UMovieSceneTrack* Track : Snapshot.MovieScene->GetTracks())
        {
            CaptureTrack(Track);
        }
        for (const FMovieSceneBinding& Binding : Bindings)
        {
            for (const UMovieSceneTrack* Track : Binding.GetTracks())
            {
                CaptureTrack(Track);
            }
        }
        return Snapshot;
    }

    void TestAnimationUnchanged(
        FAutomationTestBase& Test,
        const TCHAR* CaseName,
        const UWidgetAnimation* Animation,
        const FAnimationSnapshot& Before)
    {
        const FAnimationSnapshot After = SnapshotAnimation(Animation);
        Test.TestTrue(*FString::Printf(TEXT("%s preserves the MovieScene pointer"), CaseName),
            After.MovieScene == Before.MovieScene);
        Test.TestEqual(*FString::Printf(TEXT("%s preserves the MovieScene name"), CaseName),
            After.MovieSceneName, Before.MovieSceneName);
        Test.TestTrue(*FString::Printf(TEXT("%s preserves the playback range"), CaseName),
            After.PlaybackRange == Before.PlaybackRange);
        Test.TestTrue(*FString::Printf(TEXT("%s preserves package dirtiness"), CaseName),
            After.bPackageDirty == Before.bPackageDirty);
        Test.TestEqual(*FString::Printf(TEXT("%s preserves possessables"), CaseName),
            After.PossessableCount, Before.PossessableCount);
        Test.TestEqual(*FString::Printf(TEXT("%s preserves MovieScene bindings"), CaseName),
            After.MovieSceneBindingCount, Before.MovieSceneBindingCount);
        Test.TestEqual(*FString::Printf(TEXT("%s preserves tracks"), CaseName),
            After.TrackCount, Before.TrackCount);
        Test.TestEqual(*FString::Printf(TEXT("%s preserves sections"), CaseName),
            After.SectionCount, Before.SectionCount);
        Test.TestTrue(*FString::Printf(TEXT("%s preserves float key counts"), CaseName),
            After.FloatKeyCounts == Before.FloatKeyCounts);
        Test.TestEqual(*FString::Printf(TEXT("%s preserves widget animation bindings"), CaseName),
            After.AnimationBindingCount, Before.AnimationBindingCount);
        Test.TestTrue(*FString::Printf(TEXT("%s preserves the desired-name occupant"), CaseName),
            After.DesiredNameOccupant == Before.DesiredNameOccupant);
        Test.TestTrue(*FString::Printf(TEXT("%s preserves the desired-name occupant outer"), CaseName),
            After.DesiredNameOccupantOuter == Before.DesiredNameOccupantOuter);
        Test.TestEqual(*FString::Printf(TEXT("%s preserves the desired-name occupant name"), CaseName),
            After.DesiredNameOccupantName, Before.DesiredNameOccupantName);
    }

    TSharedPtr<FJsonObject> MakeAnimationPayload(
        const FString& WidgetPath,
        const FName& AnimationName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        Payload->SetStringField(TEXT("animationName"), AnimationName.ToString());
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationRejectedRequestsPreserveMovieSceneTest,
    "PinWright.widget.animation.RejectedRequestsPreserveMovieScene",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationRejectedRequestsPreserveMovieSceneTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(
        TEXT("WBP_AnimationRejectedAtomicity"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    UTextBlock* Target = WidgetTestFixtures::AddTextBlockToPanel(
        WBP, Root, TEXT("AnimationTarget"));
    TestNotNull(TEXT("root widget allocated"), Root);
    TestNotNull(TEXT("target widget allocated"), Target);
    if (!Root || !Target)
    {
        return false;
    }

    const FName NoSceneAnimationName(TEXT("NoSceneAnimation"));
    UWidgetAnimation* NoSceneAnimation = AddAnimationFixture(WBP, NoSceneAnimationName);
    TestNotNull(TEXT("animation without MovieScene allocated"), NoSceneAnimation);
    if (!NoSceneAnimation)
    {
        return false;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> MissingWidgetPayload =
        MakeAnimationPayload(WidgetPath, NoSceneAnimationName);
    MissingWidgetPayload->SetStringField(TEXT("widgetName"), TEXT("DefinitelyMissingWidget"));
    TestTrue(TEXT("widget.add_animation_track handler found"),
        InvokeHandlerWithCapture(TEXT("widget.add_animation_track"), MissingWidgetPayload, Capture));
    TestFalse(TEXT("missing widget is rejected"), Capture.bSuccess);
    TestEqual(TEXT("missing widget has a typed error"),
        Capture.ErrorCode, FString(TEXT("WIDGET_NOT_FOUND")));
    TestNull(TEXT("missing widget does not create a MovieScene"), NoSceneAnimation->GetMovieScene());
    TestEqual(TEXT("missing widget does not create an animation binding"),
        NoSceneAnimation->AnimationBindings.Num(), 0);

    const FName LegacyAnimationName(TEXT("LegacyAnimation"));
    UWidgetAnimation* LegacyAnimation = AddAnimationFixture(
        WBP, LegacyAnimationName, FName(TEXT("LegacyAnimation_OldMovieScene")));
    TestNotNull(TEXT("legacy animation allocated"), LegacyAnimation);
    if (!LegacyAnimation || !LegacyAnimation->GetMovieScene())
    {
        return false;
    }

    const FAnimationSnapshot BeforeMissingKeyframe = SnapshotAnimation(LegacyAnimation);
    TSharedPtr<FJsonObject> MissingKeyframeWidgetPayload =
        MakeAnimationPayload(WidgetPath, LegacyAnimationName);
    MissingKeyframeWidgetPayload->SetStringField(
        TEXT("widgetName"), TEXT("DefinitelyMissingWidget"));
    MissingKeyframeWidgetPayload->SetNumberField(TEXT("time"), 2.0);
    TestTrue(TEXT("widget.add_animation_keyframe handler found"),
        InvokeHandlerWithCapture(
            TEXT("widget.add_animation_keyframe"), MissingKeyframeWidgetPayload, Capture));
    TestFalse(TEXT("missing keyframe widget is rejected"), Capture.bSuccess);
    TestEqual(TEXT("missing keyframe widget has a typed error"),
        Capture.ErrorCode, FString(TEXT("WIDGET_NOT_FOUND")));
    TestAnimationUnchanged(
        *this, TEXT("missing keyframe widget"), LegacyAnimation, BeforeMissingKeyframe);

    const FAnimationSnapshot BeforeUnsupportedSpeed = SnapshotAnimation(LegacyAnimation);
    TSharedPtr<FJsonObject> SpeedPayload = MakeAnimationPayload(WidgetPath, LegacyAnimationName);
    SpeedPayload->SetNumberField(TEXT("speed"), 2.0);
    TestTrue(TEXT("widget.set_animation_speed handler found"),
        InvokeHandlerWithCapture(TEXT("widget.set_animation_speed"), SpeedPayload, Capture));
    TestFalse(TEXT("unsupported speed is rejected"), Capture.bSuccess);
    TestEqual(TEXT("unsupported speed has a typed error"),
        Capture.ErrorCode, FString(TEXT("NOT_SUPPORTED")));
    TestAnimationUnchanged(
        *this, TEXT("unsupported speed"), LegacyAnimation, BeforeUnsupportedSpeed);

    const FString RootlessPath = WidgetTestFixtures::MakeWidgetAssetPath(
        TEXT("WBP_AnimationNoBinding"));
    UWidgetBlueprint* RootlessWBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(RootlessPath);
    TestNotNull(TEXT("rootless widget blueprint allocated"), RootlessWBP);
    if (!RootlessWBP)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (UPackage* Package = RootlessWBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(RootlessPath);
    };

    const FName RootlessAnimationName(TEXT("RootlessAnimation"));
    UWidgetAnimation* RootlessAnimation = AddAnimationFixture(
        RootlessWBP, RootlessAnimationName, RootlessAnimationName);
    TestNotNull(TEXT("rootless animation allocated"), RootlessAnimation);
    if (!RootlessAnimation)
    {
        return false;
    }

    const FAnimationSnapshot BeforeMissingBinding = SnapshotAnimation(RootlessAnimation);
    TSharedPtr<FJsonObject> MissingBindingPayload =
        MakeAnimationPayload(RootlessPath, RootlessAnimationName);
    MissingBindingPayload->SetNumberField(TEXT("time"), 2.0);
    TestTrue(TEXT("widget.add_animation_keyframe handler found for missing binding"),
        InvokeHandlerWithCapture(
            TEXT("widget.add_animation_keyframe"), MissingBindingPayload, Capture));
    TestFalse(TEXT("missing binding is rejected"), Capture.bSuccess);
    TestEqual(TEXT("missing binding has a typed error"),
        Capture.ErrorCode, FString(TEXT("BINDING_NOT_FOUND")));
    TestAnimationUnchanged(
        *this, TEXT("missing binding"), RootlessAnimation, BeforeMissingBinding);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationAttachPreflightFailureIsAtomicTest,
    "PinWright.widget.animation.AttachPreflightFailureIsAtomic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationAttachPreflightFailureIsAtomicTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(
        TEXT("WBP_AnimationAttachPreflightAtomicity"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    UTextBlock* Target = WidgetTestFixtures::AddTextBlockToPanel(
        WBP, Root, TEXT("AnimationTarget"));
    TestNotNull(TEXT("root widget allocated"), Root);
    TestNotNull(TEXT("target widget allocated"), Target);
    if (!Root || !Target)
    {
        return false;
    }

    const FName AnimationName(TEXT("AttachPreflightAnimation"));
    UWidgetAnimation* Animation = AddAnimationFixture(
        WBP, AnimationName, FName(TEXT("AttachPreflightAnimation_OldMovieScene")));
    TestNotNull(TEXT("animation allocated"), Animation);
    if (!Animation || !Animation->GetMovieScene())
    {
        return false;
    }

    UPackage* Package = WBP->GetOutermost();
    TestNotNull(TEXT("widget package allocated"), Package);
    if (!Package)
    {
        return false;
    }
    Package->SetDirtyFlag(false);

    const FAnimationSnapshot Before = SnapshotAnimation(Animation);
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> TrackPayload = MakeAnimationPayload(WidgetPath, AnimationName);
    TrackPayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    TrackPayload->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
    {
        PinWrightWidgetAnimationTestHooks::FScopedForceTrackAttachPreflightFailure ForceFailure;
        TestTrue(TEXT("widget.add_animation_track handler found for forced preflight failure"),
            InvokeHandlerWithCapture(TEXT("widget.add_animation_track"), TrackPayload, Capture));
    }
    TestFalse(TEXT("forced track attach preflight failure is rejected"), Capture.bSuccess);
    TestEqual(TEXT("forced track attach preflight failure has a typed error"),
        Capture.ErrorCode, FString(TEXT("TRACK_CREATE_FAILED")));
    TestAnimationUnchanged(*this, TEXT("forced track attach preflight failure"), Animation, Before);

    TSharedPtr<FJsonObject> KeyframePayload = MakeAnimationPayload(WidgetPath, AnimationName);
    KeyframePayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    KeyframePayload->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
    KeyframePayload->SetNumberField(TEXT("time"), 0.5);
    KeyframePayload->SetNumberField(TEXT("value"), 0.25);
    {
        PinWrightWidgetAnimationTestHooks::FScopedForceTrackAttachPreflightFailure ForceFailure;
        TestTrue(TEXT("widget.add_animation_keyframe handler found for forced preflight failure"),
            InvokeHandlerWithCapture(TEXT("widget.add_animation_keyframe"), KeyframePayload, Capture));
    }
    TestFalse(TEXT("forced keyframe attach preflight failure is rejected"), Capture.bSuccess);
    TestEqual(TEXT("forced keyframe attach preflight failure has a typed error"),
        Capture.ErrorCode, FString(TEXT("TRACK_CREATE_FAILED")));
    TestAnimationUnchanged(
        *this, TEXT("forced keyframe attach preflight failure"), Animation, Before);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationPropertyValidationIsAtomicTest,
    "PinWright.widget.animation.PropertyValidationIsAtomic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationPropertyValidationIsAtomicTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(
        TEXT("WBP_AnimationPropertyAtomicity"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    UTextBlock* Target = WidgetTestFixtures::AddTextBlockToPanel(
        WBP, Root, TEXT("AnimationTarget"));
    TestNotNull(TEXT("root widget allocated"), Root);
    TestNotNull(TEXT("target widget allocated"), Target);
    if (!Root || !Target)
    {
        return false;
    }

    const FName AnimationName(TEXT("PropertyAnimation"));
    UWidgetAnimation* Animation = AddAnimationFixture(
        WBP, AnimationName, FName(TEXT("PropertyAnimation_OldMovieScene")));
    TestNotNull(TEXT("animation allocated"), Animation);
    if (!Animation || !Animation->GetMovieScene())
    {
        return false;
    }

    FTestResponseCapture Capture;
    const FAnimationSnapshot BeforeUnknownTrack = SnapshotAnimation(Animation);
    TSharedPtr<FJsonObject> UnknownTrackPayload = MakeAnimationPayload(WidgetPath, AnimationName);
    UnknownTrackPayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    UnknownTrackPayload->SetStringField(TEXT("propertyName"), TEXT("DefinitelyMissing"));
    TestTrue(TEXT("widget.add_animation_track handler found"),
        InvokeHandlerWithCapture(TEXT("widget.add_animation_track"), UnknownTrackPayload, Capture));
    TestFalse(TEXT("unknown track property is rejected"), Capture.bSuccess);
    TestEqual(TEXT("unknown track property has a typed error"),
        Capture.ErrorCode, FString(TEXT("PROPERTY_NOT_FOUND")));
    TestTrue(TEXT("unknown track property returns error data"), Capture.Result.IsValid());
    TestTrue(TEXT("unknown track property lists RenderOpacity as a candidate"),
        JsonStringArrayContains(Capture.Result, TEXT("candidates"), TEXT("RenderOpacity")));
    TestAnimationUnchanged(
        *this, TEXT("unknown track property"), Animation, BeforeUnknownTrack);

    const FAnimationSnapshot BeforeUnknownKeyframe = SnapshotAnimation(Animation);
    TSharedPtr<FJsonObject> UnknownKeyframePayload =
        MakeAnimationPayload(WidgetPath, AnimationName);
    UnknownKeyframePayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    UnknownKeyframePayload->SetStringField(TEXT("propertyName"), TEXT("DefinitelyMissing"));
    UnknownKeyframePayload->SetNumberField(TEXT("time"), 2.0);
    UnknownKeyframePayload->SetNumberField(TEXT("value"), 0.5);
    TestTrue(TEXT("widget.add_animation_keyframe handler found"),
        InvokeHandlerWithCapture(
            TEXT("widget.add_animation_keyframe"), UnknownKeyframePayload, Capture));
    TestFalse(TEXT("unknown keyframe property is rejected"), Capture.bSuccess);
    TestEqual(TEXT("unknown keyframe property has a typed error"),
        Capture.ErrorCode, FString(TEXT("PROPERTY_NOT_FOUND")));
    TestTrue(TEXT("unknown keyframe property lists RenderOpacity as a candidate"),
        JsonStringArrayContains(Capture.Result, TEXT("candidates"), TEXT("RenderOpacity")));
    TestAnimationUnchanged(
        *this, TEXT("unknown keyframe property"), Animation, BeforeUnknownKeyframe);

    const FAnimationSnapshot BeforeUnsupportedProperty = SnapshotAnimation(Animation);
    TSharedPtr<FJsonObject> UnsupportedPayload = MakeAnimationPayload(WidgetPath, AnimationName);
    UnsupportedPayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    UnsupportedPayload->SetStringField(TEXT("propertyName"), TEXT("Text"));
    TestTrue(TEXT("widget.add_animation_keyframe handler found for non-float property"),
        InvokeHandlerWithCapture(TEXT("widget.add_animation_keyframe"), UnsupportedPayload, Capture));
    TestFalse(TEXT("non-float property is rejected"), Capture.bSuccess);
    TestEqual(TEXT("non-float property has a typed error"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_PROPERTY")));
    TestTrue(TEXT("non-float error reports the reflected type"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("actualType")));
    TestAnimationUnchanged(
        *this, TEXT("non-float property"), Animation, BeforeUnsupportedProperty);

    const FAnimationSnapshot BeforeMalformedValue = SnapshotAnimation(Animation);
    TSharedPtr<FJsonObject> MalformedValuePayload =
        MakeAnimationPayload(WidgetPath, AnimationName);
    MalformedValuePayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    MalformedValuePayload->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
    MalformedValuePayload->SetStringField(TEXT("value"), TEXT("0.5oops"));
    TestTrue(TEXT("widget.add_animation_keyframe handler found for malformed value"),
        InvokeHandlerWithCapture(
            TEXT("widget.add_animation_keyframe"), MalformedValuePayload, Capture));
    TestFalse(TEXT("malformed value is rejected"), Capture.bSuccess);
    TestEqual(TEXT("malformed value has a typed error"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMETER")));
    TestAnimationUnchanged(
        *this, TEXT("malformed value"), Animation, BeforeMalformedValue);

    TSharedPtr<FJsonObject> ValidTrackPayload = MakeAnimationPayload(WidgetPath, AnimationName);
    ValidTrackPayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    ValidTrackPayload->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
    TestTrue(TEXT("widget.add_animation_track handler found for valid property"),
        InvokeHandlerWithCapture(TEXT("widget.add_animation_track"), ValidTrackPayload, Capture));
    TestTrue(TEXT("valid reflected float track succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Animation->GetMovieScene())
    {
        AddError(FString::Printf(TEXT("valid track failed: %s - %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }
    TestEqual(TEXT("successful commit repairs the MovieScene name"),
        Animation->GetMovieScene()->GetFName(), Animation->GetFName());

    UMovieSceneFloatTrack* StoredTrack = nullptr;
    for (const FMovieSceneBinding& Binding :
        static_cast<const UMovieScene*>(Animation->GetMovieScene())->GetBindings())
    {
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
            if (FloatTrack
                && FloatTrack->GetPropertyPath() == FName(TEXT("RenderOpacity")))
            {
                StoredTrack = FloatTrack;
                break;
            }
        }
    }
    TestNotNull(TEXT("validated property path is stored on a float track"), StoredTrack);
    if (!StoredTrack)
    {
        return false;
    }

    TSharedPtr<FJsonObject> ValidKeyframePayload =
        MakeAnimationPayload(WidgetPath, AnimationName);
    ValidKeyframePayload->SetStringField(TEXT("widgetName"), TEXT("AnimationTarget"));
    ValidKeyframePayload->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
    ValidKeyframePayload->SetNumberField(TEXT("time"), 0.5);
    ValidKeyframePayload->SetNumberField(TEXT("value"), 0.25);
    TestTrue(TEXT("widget.add_animation_keyframe handler found for valid property"),
        InvokeHandlerWithCapture(
            TEXT("widget.add_animation_keyframe"), ValidKeyframePayload, Capture));
    TestTrue(TEXT("valid reflected float keyframe succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("valid keyframe failed: %s - %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    const TArray<UMovieSceneSection*>& Sections = StoredTrack->GetAllSections();
    UMovieSceneFloatSection* FloatSection = Sections.Num() > 0
        ? Cast<UMovieSceneFloatSection>(Sections[0])
        : nullptr;
    TestNotNull(TEXT("valid keyframe has a float section"), FloatSection);
    TestEqual(TEXT("valid keyframe commits exactly one key"),
        FloatSection ? FloatSection->GetChannel().GetNumKeys() : 0,
        1);

    return true;
}
