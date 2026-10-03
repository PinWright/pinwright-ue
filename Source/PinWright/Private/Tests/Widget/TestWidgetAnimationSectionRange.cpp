// Copyright (c) 2026 Alexander Penkin. MIT License.

// widget.get_animation_section_ranges / widget.set_animation_section_range
// (board F-widget-anim-section-range-verb).
//
// The defect shape: a section [0, K) whose final key sits AT K never evaluates that key, so the
// widget stops one pose short. The fix is to open the section's end. These tests build that
// shape on a compiled Widget Blueprint, open it through the verb, and check the three things a
// caller relies on: the source section changed (read directly, not from the response), the
// edit is one undoable transaction, and the compiled `<Anim>_INST` copy is reported stale until
// a compile and current after one.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CanvasPanel.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace TestWidgetAnimationSectionRangeHelpers
{
    const TCHAR* const AnimName = TEXT("HoverAnim");
    const TCHAR* const TargetName = TEXT("AnimTarget");

    struct FFixture
    {
        FString WidgetPath;
        UWidgetBlueprint* WBP = nullptr;
        UMovieSceneFloatSection* Section = nullptr;
        FFrameNumber LastKey;
        double TickRate = 0.0;
        double DisplayRate = 0.0;
    };

    TSharedPtr<FJsonObject> MakePayload(const FFixture& Fixture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), Fixture.WidgetPath);
        Payload->SetStringField(TEXT("animationName"), AnimName);
        return Payload;
    }

    // Compilable WBP (CreateBlueprint, transient package) + a RenderOpacity track keyed at 0 and
    // 0.1 s through the real verbs, then the section narrowed to [0, lastKey) - the defect shape.
    bool BuildFixture(FAutomationTestBase& Test, const TCHAR* Prefix, FFixture& Out)
    {
        Out.WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(Prefix);
        UPackage* Package = CreatePackage(*Out.WidgetPath);
        if (!Package)
        {
            return false;
        }
        Package->SetFlags(RF_Transient);
        Out.WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            UUserWidget::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(Out.WidgetPath)),
            BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
        if (!Test.TestNotNull(TEXT("fixture: widget blueprint created"), Out.WBP) || !Out.WBP->WidgetTree)
        {
            return false;
        }
        UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(Out.WBP);
        if (!Test.TestNotNull(TEXT("fixture: target widget"), WidgetTestFixtures::AddTextBlockToPanel(Out.WBP, Root, TargetName)))
        {
            return false;
        }

        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Create = MakePayload(Out);
        Create->SetNumberField(TEXT("duration"), 0.1);
        InvokeHandlerWithCapture(TEXT("widget.create_widget_animation"), Create, Capture);
        if (!Test.TestTrue(TEXT("fixture: animation created"), Capture.bSuccess))
        {
            return false;
        }
        for (const double Time : {0.0, 0.1})
        {
            TSharedPtr<FJsonObject> Key = MakePayload(Out);
            Key->SetStringField(TEXT("widgetName"), TargetName);
            Key->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
            Key->SetNumberField(TEXT("time"), Time);
            Key->SetNumberField(TEXT("value"), Time > 0.0 ? 1.0 : 0.0);
            InvokeHandlerWithCapture(TEXT("widget.add_animation_keyframe"), Key, Capture);
            if (!Test.TestTrue(TEXT("fixture: keyframe added"), Capture.bSuccess))
            {
                return false;
            }
        }

        UWidgetAnimation* Animation = Out.WBP->Animations.Num() == 1 ? Out.WBP->Animations[0].Get() : nullptr;
        UMovieScene* MovieScene = Animation ? Animation->GetMovieScene() : nullptr;
        if (!Test.TestNotNull(TEXT("fixture: animation with a MovieScene"), MovieScene))
        {
            return false;
        }
        const TArray<FMovieSceneBinding>& Bindings = static_cast<const UMovieScene*>(MovieScene)->GetBindings();
        if (!Test.TestTrue(TEXT("fixture: one binding with one track"),
                Bindings.Num() == 1 && Bindings[0].GetTracks().Num() == 1
                && Bindings[0].GetTracks()[0]->GetAllSections().Num() == 1))
        {
            return false;
        }
        Out.Section = Cast<UMovieSceneFloatSection>(Bindings[0].GetTracks()[0]->GetAllSections()[0]);
        if (!Test.TestTrue(TEXT("fixture: float section with two keys"),
                Out.Section && Out.Section->GetChannel().GetTimes().Num() == 2))
        {
            return false;
        }
        Out.LastKey = Out.Section->GetChannel().GetTimes().Last();
        Out.TickRate = MovieScene->GetTickResolution().AsDecimal();
        Out.DisplayRate = MovieScene->GetDisplayRate().AsDecimal();
        Out.Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), Out.LastKey));
        return Test.TestTrue(TEXT("fixture: section ends exclusively at the final key"),
            Out.Section->GetRange().HasUpperBound() && Out.Section->GetRange().GetUpperBoundValue() == Out.LastKey
            && !Out.Section->GetRange().Contains(Out.LastKey));
    }

    TSharedPtr<FJsonObject> FirstSection(const FTestResponseCapture& Capture)
    {
        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("sections"), Sections)
            && Sections->Num() == 1)
        {
            return (*Sections)[0]->AsObject();
        }
        return nullptr;
    }

    bool GetBoolOr(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool bDefault)
    {
        bool bValue = bDefault;
        return Obj.IsValid() && Obj->TryGetBoolField(Field, bValue) ? bValue : bDefault;
    }

    double GetRangeNumber(const TSharedPtr<FJsonObject>& Section, const TCHAR* RangeField, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Range = nullptr;
        double Value = -1.0;
        if (Section.IsValid() && Section->TryGetObjectField(RangeField, Range))
        {
            (*Range)->TryGetNumberField(Field, Value);
        }
        return Value;
    }

    bool RangeEndOpen(const TSharedPtr<FJsonObject>& Section, const TCHAR* RangeField)
    {
        const TSharedPtr<FJsonObject>* Range = nullptr;
        bool bBounded = true;
        return Section.IsValid() && Section->TryGetObjectField(RangeField, Range)
            && (*Range)->TryGetBoolField(TEXT("endBounded"), bBounded) && !bBounded;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationSectionRangeOpenEndTest,
    "PinWright.widget.animation_section_range.OpenEndReachesFinalKeyAfterCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationSectionRangeOpenEndTest::RunTest(const FString& Parameters)
{
    using namespace TestWidgetAnimationSectionRangeHelpers;
    FFixture Fixture;
    ON_SCOPE_EXIT
    {
        if (Fixture.WBP)
        {
            Fixture.WBP->GetOutermost()->SetDirtyFlag(false);
        }
        if (!Fixture.WidgetPath.IsEmpty())
        {
            CleanupTestAsset(Fixture.WidgetPath);
        }
    };
    if (!BuildFixture(*this, TEXT("WBP_SectionRangeOpenEnd"), Fixture))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(Fixture.WBP);

    // Read: the defect is visible, and the compiled copy agrees with the source.
    FTestResponseCapture Capture;
    TestTrue(TEXT("get handler registered"),
        InvokeHandlerWithCapture(TEXT("widget.get_animation_section_ranges"), MakePayload(Fixture), Capture));
    TestTrue(TEXT("get succeeds"), Capture.bSuccess);
    TSharedPtr<FJsonObject> Read = FirstSection(Capture);
    TestTrue(TEXT("get reports exactly one section"), Read.IsValid());
    TestFalse(TEXT("get flags the final key as outside the section"), GetBoolOr(Read, TEXT("keysInsideSection"), true));
    TestEqual(TEXT("get reports the bounded end"), GetRangeNumber(Read, TEXT("range"), TEXT("endFrame")),
        static_cast<double>(Fixture.LastKey.Value));
    TestTrue(TEXT("compiled copy matches before the edit"), GetBoolOr(Read, TEXT("compiledCopyMatches"), false));
    TestFalse(TEXT("no compile required before the edit"), GetBoolOr(Capture.Result, TEXT("compileRequired"), true));

    // Write: open the end.
    TSharedPtr<FJsonObject> Set = MakePayload(Fixture);
    Set->SetStringField(TEXT("widgetName"), TargetName);
    Set->SetStringField(TEXT("propertyName"), TEXT("RenderOpacity"));
    Set->SetStringField(TEXT("end"), TEXT("unbounded"));
    TestTrue(TEXT("set handler registered"),
        InvokeHandlerWithCapture(TEXT("widget.set_animation_section_range"), Set, Capture));
    TestTrue(TEXT("set succeeds"), Capture.bSuccess);
    const TSharedPtr<FJsonObject> Written = FirstSection(Capture);
    TestEqual(TEXT("set echoes the old end"), GetRangeNumber(Written, TEXT("before"), TEXT("endFrame")),
        static_cast<double>(Fixture.LastKey.Value));
    TestTrue(TEXT("set reads back an open end"), RangeEndOpen(Written, TEXT("range")));
    TestTrue(TEXT("set reports applied"), GetBoolOr(Written, TEXT("applied"), false));
    TestTrue(TEXT("final key now inside the section"), GetBoolOr(Written, TEXT("keysInsideSection"), false));
    TestFalse(TEXT("compiled copy is reported stale"), GetBoolOr(Written, TEXT("compiledCopyMatches"), true));
    TestTrue(TEXT("compile reported as required"), GetBoolOr(Capture.Result, TEXT("compileRequired"), false));
    TestFalse(TEXT("source section end is open (direct read)"), Fixture.Section->GetRange().HasUpperBound());
    TestTrue(TEXT("source section start kept"),
        Fixture.Section->GetRange().HasLowerBound() && Fixture.Section->GetRange().GetLowerBoundValue() == FFrameNumber(0));

    // Undo restores the bounded end in one step.
    TestTrue(TEXT("undo succeeds"), GEditor && GEditor->UndoTransaction(/*bCanRedo=*/false));
    TestTrue(TEXT("undo restores the exclusive end at the final key"),
        Fixture.Section->GetRange().HasUpperBound() && Fixture.Section->GetRange().GetUpperBoundValue() == Fixture.LastKey);

    // Re-apply, compile, and the compiled copy carries the open end.
    InvokeHandlerWithCapture(TEXT("widget.set_animation_section_range"), Set, Capture);
    TestTrue(TEXT("re-apply succeeds"), Capture.bSuccess);
    FKismetEditorUtilities::CompileBlueprint(Fixture.WBP);
    InvokeHandlerWithCapture(TEXT("widget.get_animation_section_ranges"), MakePayload(Fixture), Capture);
    Read = FirstSection(Capture);
    TestTrue(TEXT("compiled copy matches after compile"), GetBoolOr(Read, TEXT("compiledCopyMatches"), false));
    TestTrue(TEXT("compiled copy end is open"), RangeEndOpen(Read, TEXT("compiledRange")));
    TestFalse(TEXT("no compile required after compile"), GetBoolOr(Capture.Result, TEXT("compileRequired"), true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAnimationSectionRangeUnitsAndRefusalsTest,
    "PinWright.widget.animation_section_range.UnitsAndRefusals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAnimationSectionRangeUnitsAndRefusalsTest::RunTest(const FString& Parameters)
{
    using namespace TestWidgetAnimationSectionRangeHelpers;
    FFixture Fixture;
    ON_SCOPE_EXIT
    {
        if (Fixture.Section)
        {
            Fixture.Section->SetIsLocked(false);
            if (UMovieScene* Scene = Fixture.Section->GetTypedOuter<UMovieScene>())
            {
                Scene->SetReadOnly(false);
            }
        }
        if (Fixture.WBP)
        {
            Fixture.WBP->GetOutermost()->SetDirtyFlag(false);
        }
        if (!Fixture.WidgetPath.IsEmpty())
        {
            CleanupTestAsset(Fixture.WidgetPath);
        }
    };
    if (!BuildFixture(*this, TEXT("WBP_SectionRangeRefusals"), Fixture))
    {
        return false;
    }
    FTestResponseCapture Capture;

    // The read verb leaves the package's dirty flag exactly as it found it.
    Fixture.WBP->GetOutermost()->SetDirtyFlag(false);
    InvokeHandlerWithCapture(TEXT("widget.get_animation_section_ranges"), MakePayload(Fixture), Capture);
    TestTrue(TEXT("get succeeds"), Capture.bSuccess);
    TestFalse(TEXT("get leaves the package clean"), Fixture.WBP->GetOutermost()->IsDirty());
    // GetTrackName() would report the property name for this float property track (and "None"
    // for non-property tracks); trackName is the track's object name.
    const UMovieSceneTrack* Track = Fixture.Section->GetTypedOuter<UMovieSceneTrack>();
    TestTrue(TEXT("fixture section has an owning track"), Track != nullptr);
    FString TrackName;
    const TSharedPtr<FJsonObject> ReadSection = FirstSection(Capture);
    TestTrue(TEXT("get reports trackName"), ReadSection.IsValid() && ReadSection->TryGetStringField(TEXT("trackName"), TrackName));
    TestEqual(TEXT("trackName is the track object name"), TrackName, Track ? Track->GetName() : FString());
    // propertyName also matches that track object name (GetTrackName() would be "RenderOpacity").
    TSharedPtr<FJsonObject> ByTrackName = MakePayload(Fixture);
    ByTrackName->SetStringField(TEXT("propertyName"), Track ? Track->GetName() : FString());
    InvokeHandlerWithCapture(TEXT("widget.get_animation_section_ranges"), ByTrackName, Capture);
    TestTrue(TEXT("propertyName = track object name succeeds"), Capture.bSuccess);
    TestTrue(TEXT("propertyName = track object name matches exactly one section"), FirstSection(Capture).IsValid());

    // Units convert against the animation's own rates.
    TSharedPtr<FJsonObject> Seconds = MakePayload(Fixture);
    Seconds->SetNumberField(TEXT("end"), 0.25);
    Seconds->SetStringField(TEXT("units"), TEXT("seconds"));
    InvokeHandlerWithCapture(TEXT("widget.set_animation_section_range"), Seconds, Capture);
    TestTrue(TEXT("seconds end succeeds"), Capture.bSuccess);
    TestEqual(TEXT("seconds end lands at round(0.25 * tickRate)"),
        Fixture.Section->GetRange().GetUpperBoundValue().Value, FMath::RoundToInt32(0.25 * Fixture.TickRate));
    TestEqual(TEXT("seconds requested.endFrame echoes the converted ticks"),
        GetRangeNumber(FirstSection(Capture), TEXT("requested"), TEXT("endFrame")),
        static_cast<double>(FMath::RoundToInt32(0.25 * Fixture.TickRate)));

    TSharedPtr<FJsonObject> Frames = MakePayload(Fixture);
    Frames->SetNumberField(TEXT("end"), 6);
    Frames->SetStringField(TEXT("units"), TEXT("displayFrames"));
    InvokeHandlerWithCapture(TEXT("widget.set_animation_section_range"), Frames, Capture);
    TestTrue(TEXT("displayFrames end succeeds"), Capture.bSuccess);
    TestEqual(TEXT("displayFrames end lands at round(6 * tickRate / displayRate)"),
        Fixture.Section->GetRange().GetUpperBoundValue().Value,
        FMath::RoundToInt32(6.0 * Fixture.TickRate / Fixture.DisplayRate));

    // Every refusal leaves the section exactly as it was.
    const TRange<FFrameNumber> Before = Fixture.Section->GetRange();
    auto ExpectRefusal = [&](const TCHAR* What, const TSharedPtr<FJsonObject>& Payload, const TCHAR* Code)
    {
        InvokeHandlerWithCapture(TEXT("widget.set_animation_section_range"), Payload, Capture);
        TestFalse(*FString::Printf(TEXT("%s is refused"), What), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s error code"), What), Capture.ErrorCode, FString(Code));
        TestTrue(*FString::Printf(TEXT("%s leaves the range unchanged"), What), Fixture.Section->GetRange() == Before);
    };

    TSharedPtr<FJsonObject> Inverted = MakePayload(Fixture);
    Inverted->SetNumberField(TEXT("start"), 5000);
    Inverted->SetNumberField(TEXT("end"), 100);
    ExpectRefusal(TEXT("start after end"), Inverted, TEXT("INVALID_PARAMETER"));

    TSharedPtr<FJsonObject> Fractional = MakePayload(Fixture);
    Fractional->SetNumberField(TEXT("end"), 10.5);
    ExpectRefusal(TEXT("fractional ticks"), Fractional, TEXT("INVALID_PARAMETER"));

    TSharedPtr<FJsonObject> BadWord = MakePayload(Fixture);
    BadWord->SetStringField(TEXT("end"), TEXT("forever"));
    ExpectRefusal(TEXT("unknown bound word"), BadWord, TEXT("INVALID_PARAMETER"));

    TSharedPtr<FJsonObject> BadUnits = MakePayload(Fixture);
    BadUnits->SetStringField(TEXT("end"), TEXT("unbounded"));
    BadUnits->SetStringField(TEXT("units"), TEXT("frames"));
    ExpectRefusal(TEXT("unknown units"), BadUnits, TEXT("INVALID_PARAMETER"));

    TSharedPtr<FJsonObject> NoMatch = MakePayload(Fixture);
    NoMatch->SetStringField(TEXT("end"), TEXT("unbounded"));
    NoMatch->SetStringField(TEXT("propertyName"), TEXT("NoSuchProperty"));
    ExpectRefusal(TEXT("no matching section"), NoMatch, TEXT("SECTION_NOT_FOUND"));

    ExpectRefusal(TEXT("neither start nor end"), MakePayload(Fixture), TEXT("MISSING_PARAMETER"));

    // A read-only MovieScene makes SetRange a silent no-op (TryModify); refuse it up front.
    UMovieScene* Scene = Fixture.Section->GetTypedOuter<UMovieScene>();
    TestTrue(TEXT("fixture section has an owning MovieScene"), Scene != nullptr);
    if (Scene)
    {
        Scene->SetReadOnly(true);
        TSharedPtr<FJsonObject> ReadOnly = MakePayload(Fixture);
        ReadOnly->SetStringField(TEXT("end"), TEXT("unbounded"));
        ExpectRefusal(TEXT("read-only MovieScene"), ReadOnly, TEXT("SECTION_READ_ONLY"));
        Scene->SetReadOnly(false);
    }

    Fixture.Section->SetIsLocked(true);
    TSharedPtr<FJsonObject> Locked = MakePayload(Fixture);
    Locked->SetStringField(TEXT("end"), TEXT("unbounded"));
    ExpectRefusal(TEXT("locked section"), Locked, TEXT("SECTION_READ_ONLY"));
    return true;
}
