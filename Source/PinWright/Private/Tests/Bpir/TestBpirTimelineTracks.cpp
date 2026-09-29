// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirTimelineTracks.cpp
// Regression tests for B-decompile-drops-timeline-tracks: a Timeline's settings and its float,
// vector, linear color and event tracks survive decompile then recompile, a replace-mode
// recompile keeps them, and a name collision or malformed track is a compile error instead of a
// silent bind to another template.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "Engine/TimelineTemplate.h"
#include "K2Node_Timeline.h"

using namespace CompilerTestUtils;

namespace BpirTimelineTracksTest
{
    const FName TimelineName(TEXT("FadeIn"));

    FString TimelineProgram(const FString& Args)
    {
        return FString::Printf(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    %%tl = timeline FadeIn(%s) [update -> @tick]\n")
            TEXT("\n")
            TEXT("@tick:\n")
            TEXT("    call PrintString(InString: \"Tick\")\n")
            TEXT("}"), *Args);
    }

    bool CompileOrReport(FAutomationTestBase& Test, UBlueprint* BP, const FString& Code,
        EBpirCompileMode Mode = EBpirCompileMode::Default)
    {
        FBpirCompiler Compiler(BP);
        const FCompileResult Result = Compiler.Compile(Code, Mode);
        for (const FCompileError& Err : Result.Errors)
        {
            Test.AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
        return Result.bSuccess;
    }

    FString DecompileOrReport(FAutomationTestBase& Test, UBlueprint* BP)
    {
        FBpirDecompiler Decompiler(BP);
        const FBpirDecompileResult Result = Decompiler.Decompile();
        Test.TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
        return Result.BpirText;
    }

    UK2Node_Timeline* FindTimelineNode(UBlueprint* BP)
    {
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_Timeline* Timeline = Cast<UK2Node_Timeline>(Node);
                if (Timeline && Timeline->TimelineName == TimelineName)
                {
                    return Timeline;
                }
            }
        }
        return nullptr;
    }

    // Name, external flag and curve channels of one track, whatever its kind.
    struct FTrackView
    {
        FName Name;
        bool bExternal = false;
        const UCurveBase* Curve = nullptr;
        TArray<const FRichCurve*> Channels;
    };

    FTrackView ViewTrack(const UTimelineTemplate* Template, const FTTTrackId& Id)
    {
        FTrackView View;
        switch (Id.TrackType)
        {
        case FTTTrackBase::TT_Event:
        {
            const FTTEventTrack& Track = Template->EventTracks[Id.TrackIndex];
            View = { Track.GetTrackName(), Track.bIsExternalCurve, Track.CurveKeys.Get(), {} };
            if (Track.CurveKeys) { View.Channels.Add(&Track.CurveKeys->FloatCurve); }
            break;
        }
        case FTTTrackBase::TT_FloatInterp:
        {
            const FTTFloatTrack& Track = Template->FloatTracks[Id.TrackIndex];
            View = { Track.GetTrackName(), Track.bIsExternalCurve, Track.CurveFloat.Get(), {} };
            if (Track.CurveFloat) { View.Channels.Add(&Track.CurveFloat->FloatCurve); }
            break;
        }
        case FTTTrackBase::TT_VectorInterp:
        {
            const FTTVectorTrack& Track = Template->VectorTracks[Id.TrackIndex];
            View = { Track.GetTrackName(), Track.bIsExternalCurve, Track.CurveVector.Get(), {} };
            for (int32 Channel = 0; Track.CurveVector && Channel < 3; ++Channel) { View.Channels.Add(&Track.CurveVector->FloatCurves[Channel]); }
            break;
        }
        case FTTTrackBase::TT_LinearColorInterp:
        {
            const FTTLinearColorTrack& Track = Template->LinearColorTracks[Id.TrackIndex];
            View = { Track.GetTrackName(), Track.bIsExternalCurve, Track.CurveLinearColor.Get(), {} };
            for (int32 Channel = 0; Track.CurveLinearColor && Channel < 4; ++Channel) { View.Channels.Add(&Track.CurveLinearColor->FloatCurves[Channel]); }
            break;
        }
        default:
            break;
        }
        return View;
    }

    FTrackView ViewDisplayTrack(UTimelineTemplate* Template, int32 DisplayIndex)
    {
        return ViewTrack(Template, Template->GetDisplayTrackId(DisplayIndex));
    }

    void ExpectSameKeys(FAutomationTestBase& Test, const FString& What, const FRichCurve& Expected, const FRichCurve& Actual)
    {
        if (!Test.TestEqual(*(What + TEXT(" key count")), Actual.Keys.Num(), Expected.Keys.Num()))
        {
            return;
        }
        for (int32 Index = 0; Index < Expected.Keys.Num(); ++Index)
        {
            const FRichCurveKey& E = Expected.Keys[Index];
            const FRichCurveKey& A = Actual.Keys[Index];
            const FString Key = FString::Printf(TEXT("%s key %d"), *What, Index);
            Test.TestEqual(*(Key + TEXT(" time")), A.Time, E.Time);
            Test.TestEqual(*(Key + TEXT(" value")), A.Value, E.Value);
            Test.TestEqual(*(Key + TEXT(" interp")), static_cast<int32>(A.InterpMode.GetValue()), static_cast<int32>(E.InterpMode.GetValue()));
            Test.TestEqual(*(Key + TEXT(" tangent mode")), static_cast<int32>(A.TangentMode.GetValue()), static_cast<int32>(E.TangentMode.GetValue()));
            Test.TestEqual(*(Key + TEXT(" weight mode")), static_cast<int32>(A.TangentWeightMode.GetValue()), static_cast<int32>(E.TangentWeightMode.GetValue()));
            Test.TestEqual(*(Key + TEXT(" arrive tangent")), A.ArriveTangent, E.ArriveTangent, 1e-4f);
            Test.TestEqual(*(Key + TEXT(" leave tangent")), A.LeaveTangent, E.LeaveTangent, 1e-4f);
            Test.TestEqual(*(Key + TEXT(" arrive weight")), A.ArriveTangentWeight, E.ArriveTangentWeight, 1e-4f);
            Test.TestEqual(*(Key + TEXT(" leave weight")), A.LeaveTangentWeight, E.LeaveTangentWeight, 1e-4f);
        }
    }

    // Compares settings and every track in display order, keys included.
    void ExpectSameTemplate(FAutomationTestBase& Test, UTimelineTemplate* Expected, UTimelineTemplate* Actual)
    {
        Test.TestEqual(TEXT("length"), Actual->TimelineLength, Expected->TimelineLength);
        Test.TestEqual(TEXT("length mode"), static_cast<int32>(Actual->LengthMode.GetValue()), static_cast<int32>(Expected->LengthMode.GetValue()));
        Test.TestEqual(TEXT("autoplay"), Actual->bAutoPlay != 0, Expected->bAutoPlay != 0);
        Test.TestEqual(TEXT("loop"), Actual->bLoop != 0, Expected->bLoop != 0);
        Test.TestEqual(TEXT("replicated"), Actual->bReplicated != 0, Expected->bReplicated != 0);
        Test.TestEqual(TEXT("ignore time dilation"), Actual->bIgnoreTimeDilation != 0, Expected->bIgnoreTimeDilation != 0);
        if (!Test.TestEqual(TEXT("display track count"), Actual->GetNumDisplayTracks(), Expected->GetNumDisplayTracks()))
        {
            return;
        }
        for (int32 Index = 0; Index < Expected->GetNumDisplayTracks(); ++Index)
        {
            const FTTTrackId ExpectedId = Expected->GetDisplayTrackId(Index);
            const FTTTrackId ActualId = Actual->GetDisplayTrackId(Index);
            const FString What = FString::Printf(TEXT("display track %d"), Index);
            if (!Test.TestEqual(*(What + TEXT(" kind")), ActualId.TrackType, ExpectedId.TrackType))
            {
                continue;
            }
            const FTrackView E = ViewTrack(Expected, ExpectedId);
            const FTrackView A = ViewTrack(Actual, ActualId);
            Test.TestEqual(*(What + TEXT(" name")), A.Name.ToString(), E.Name.ToString());
            Test.TestEqual(*(What + TEXT(" external")), A.bExternal, E.bExternal);
            if (!Test.TestEqual(*(What + TEXT(" channel count")), A.Channels.Num(), E.Channels.Num()))
            {
                continue;
            }
            for (int32 Channel = 0; Channel < E.Channels.Num(); ++Channel)
            {
                ExpectSameKeys(Test, FString::Printf(TEXT("%s channel %d"), *What, Channel), *E.Channels[Channel], *A.Channels[Channel]);
            }
        }
    }

    struct FRoundTrip
    {
        UBlueprint* SourceBlueprint = nullptr;
        UBlueprint* RebuiltBlueprint = nullptr;
        UTimelineTemplate* Source = nullptr;
        UTimelineTemplate* Rebuilt = nullptr;
        FString Text;
    };

    // Compiles Program into a fresh Blueprint, decompiles it, recompiles that text into a second
    // fresh Blueprint, and compares the two templates.
    bool RoundTrip(FAutomationTestBase& Test, const FString& Program, FRoundTrip& Out)
    {
        Out.SourceBlueprint = CreateTransientTestBP(TEXT("TimelineTracksSourceBP"));
        if (!Test.TestNotNull(TEXT("Source Blueprint"), Out.SourceBlueprint)
            || !CompileOrReport(Test, Out.SourceBlueprint, Program))
        {
            return false;
        }
        Out.Source = Out.SourceBlueprint->FindTimelineTemplateByVariableName(TimelineName);
        Out.Text = DecompileOrReport(Test, Out.SourceBlueprint);

        Out.RebuiltBlueprint = CreateTransientTestBP(TEXT("TimelineTracksRebuiltBP"));
        if (!Test.TestNotNull(TEXT("Rebuilt Blueprint"), Out.RebuiltBlueprint)
            || !CompileOrReport(Test, Out.RebuiltBlueprint, Out.Text))
        {
            Test.AddInfo(FString::Printf(TEXT("Decompiled text:\n%s"), *Out.Text));
            return false;
        }
        Out.Rebuilt = Out.RebuiltBlueprint->FindTimelineTemplateByVariableName(TimelineName);
        if (!Test.TestNotNull(TEXT("Source template"), Out.Source)
            || !Test.TestNotNull(TEXT("Rebuilt template"), Out.Rebuilt))
        {
            return false;
        }
        ExpectSameTemplate(Test, Out.Source, Out.Rebuilt);
        return true;
    }

    // Compiles a program that must fail, and checks the error names the problem and nothing was left behind.
    void ExpectRejected(FAutomationTestBase& Test, const FString& Args, const FString& ErrorFragment)
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("TimelineTracksRejectedBP"));
        if (!Test.TestNotNull(TEXT("Blueprint"), BP))
        {
            return;
        }
        FBpirCompiler Compiler(BP);
        const FCompileResult Result = Compiler.Compile(TimelineProgram(Args));
        const FString What = FString::Printf(TEXT("[%s]"), *Args);
        Test.TestFalse(*(What + TEXT(" compile fails")), Result.bSuccess);
        Test.TestTrue(*FString::Printf(TEXT("%s error mentions '%s'"), *What, *ErrorFragment), ErrorsContain(Result.Errors, ErrorFragment));
        Test.TestEqual(*(What + TEXT(" leaves no timeline template")), BP->Timelines.Num(), 0);
        Test.TestEqual(*(What + TEXT(" leaves no timeline node")), CountNodesOfType<UK2Node_Timeline>(BP), 0);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineFloatTrackRoundTripsTest,
    "PinWright.bpir.timeline_tracks.FloatTrackRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineFloatTrackRoundTripsTest::RunTest(const FString& Parameters)
{
    BpirTimelineTracksTest::FRoundTrip Trip;
    if (!BpirTimelineTracksTest::RoundTrip(*this, BpirTimelineTracksTest::TimelineProgram(TEXT(
        "Alpha: float_curve((0, 0), (0.5, 0.25, constant), (1, 1, cubic, user, 2, -3), (2, 0.1, cubic, break, 0.5, 1.5, both, 0.3, 0.7))")), Trip))
    {
        return false;
    }

    // The compile side, checked against the text rather than against the decompiler.
    TestEqual(TEXT("One float track"), Trip.Source->FloatTracks.Num(), 1);
    TestEqual(TEXT("One display track"), Trip.Source->GetNumDisplayTracks(), 1);
    const BpirTimelineTracksTest::FTrackView Alpha = BpirTimelineTracksTest::ViewDisplayTrack(Trip.Source, 0);
    TestEqual(TEXT("Track name"), Alpha.Name.ToString(), TEXT("Alpha"));
    if (TestEqual(TEXT("Alpha channel count"), Alpha.Channels.Num(), 1)
        && TestEqual(TEXT("Alpha key count"), Alpha.Channels[0]->Keys.Num(), 4))
    {
        const TArray<FRichCurveKey>& Keys = Alpha.Channels[0]->Keys;
        TestEqual(TEXT("Key 0 is linear"), static_cast<int32>(Keys[0].InterpMode.GetValue()), static_cast<int32>(RCIM_Linear));
        TestEqual(TEXT("Key 1 is constant"), static_cast<int32>(Keys[1].InterpMode.GetValue()), static_cast<int32>(RCIM_Constant));
        TestEqual(TEXT("Key 2 is cubic user"), static_cast<int32>(Keys[2].TangentMode.GetValue()), static_cast<int32>(RCTM_User));
        TestEqual(TEXT("Key 2 arrive tangent"), Keys[2].ArriveTangent, 2.f);
        TestEqual(TEXT("Key 2 leave tangent"), Keys[2].LeaveTangent, -3.f);
        TestEqual(TEXT("Key 3 is cubic break"), static_cast<int32>(Keys[3].TangentMode.GetValue()), static_cast<int32>(RCTM_Break));
        TestEqual(TEXT("Key 3 weight mode"), static_cast<int32>(Keys[3].TangentWeightMode.GetValue()), static_cast<int32>(RCTWM_WeightedBoth));
        TestEqual(TEXT("Key 3 leave weight"), Keys[3].LeaveTangentWeight, 0.7f);
    }
    UK2Node_Timeline* Node = BpirTimelineTracksTest::FindTimelineNode(Trip.SourceBlueprint);
    TestTrue(TEXT("Timeline node exposes the Alpha output pin"), Node && Node->FindPin(TEXT("Alpha")) != nullptr);
    TestTrue(TEXT("Decompiled text carries the float track"), Trip.Text.Contains(TEXT("Alpha: float_curve((0, 0), (0.5, 0.25, constant)")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineVectorTrackRoundTripsTest,
    "PinWright.bpir.timeline_tracks.VectorTrackRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineVectorTrackRoundTripsTest::RunTest(const FString& Parameters)
{
    BpirTimelineTracksTest::FRoundTrip Trip;
    if (!BpirTimelineTracksTest::RoundTrip(*this, BpirTimelineTracksTest::TimelineProgram(TEXT("Offset: vector_curve(x((0, 0), (1, 10)), z((0.5, -2, cubic)))")), Trip))
    {
        return false;
    }

    TestEqual(TEXT("One vector track"), Trip.Source->VectorTracks.Num(), 1);
    const BpirTimelineTracksTest::FTrackView Offset = BpirTimelineTracksTest::ViewDisplayTrack(Trip.Source, 0);
    if (TestEqual(TEXT("Offset channel count"), Offset.Channels.Num(), 3))
    {
        TestEqual(TEXT("x keys"), Offset.Channels[0]->Keys.Num(), 2);
        TestEqual(TEXT("y keys"), Offset.Channels[1]->Keys.Num(), 0);
        TestEqual(TEXT("z keys"), Offset.Channels[2]->Keys.Num(), 1);
    }
    UK2Node_Timeline* Node = BpirTimelineTracksTest::FindTimelineNode(Trip.SourceBlueprint);
    TestTrue(TEXT("Timeline node exposes the Offset output pin"), Node && Node->FindPin(TEXT("Offset")) != nullptr);
    TestTrue(TEXT("Decompiled text carries the vector track without the empty channel"),
        Trip.Text.Contains(TEXT("Offset: vector_curve(x((0, 0), (1, 10)), z((0.5, -2, cubic, auto")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineColorTrackRoundTripsTest,
    "PinWright.bpir.timeline_tracks.ColorTrackRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineColorTrackRoundTripsTest::RunTest(const FString& Parameters)
{
    BpirTimelineTracksTest::FRoundTrip Trip;
    if (!BpirTimelineTracksTest::RoundTrip(*this, BpirTimelineTracksTest::TimelineProgram(TEXT("Tint: color_curve(r((0, 1)), g((0, 0.5)), b((0, 0.25)), a((0, 1), (1, 0)))")), Trip))
    {
        return false;
    }

    TestEqual(TEXT("One linear color track"), Trip.Source->LinearColorTracks.Num(), 1);
    const BpirTimelineTracksTest::FTrackView Tint = BpirTimelineTracksTest::ViewDisplayTrack(Trip.Source, 0);
    if (TestEqual(TEXT("Tint channel count"), Tint.Channels.Num(), 4))
    {
        TestEqual(TEXT("a keys"), Tint.Channels[3]->Keys.Num(), 2);
        TestEqual(TEXT("g value"), Tint.Channels[1]->Keys.Num() > 0 ? Tint.Channels[1]->Keys[0].Value : -1.f, 0.5f);
    }
    TestTrue(TEXT("Decompiled text carries the color track"), Trip.Text.Contains(TEXT("Tint: color_curve(r((0, 1)), g((0, 0.5))")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineEventTrackRoundTripsTest,
    "PinWright.bpir.timeline_tracks.EventTrackRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineEventTrackRoundTripsTest::RunTest(const FString& Parameters)
{
    BpirTimelineTracksTest::FRoundTrip Trip;
    if (!BpirTimelineTracksTest::RoundTrip(*this,
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %tl = timeline FadeIn(Beep: event_curve((0.25, 0), (0.75, 0))) [update -> @tick, Beep -> @beep]\n")
        TEXT("\n")
        TEXT("@tick:\n")
        TEXT("    call PrintString(InString: \"Tick\")\n")
        TEXT("    exec -> @done\n")
        TEXT("\n")
        TEXT("@beep:\n")
        TEXT("    call PrintString(InString: \"Beep\")\n")
        TEXT("    exec -> @done\n")
        TEXT("\n")
        TEXT("@done:\n")
        TEXT("}"), Trip))
    {
        return false;
    }

    if (TestEqual(TEXT("One event track"), Trip.Source->EventTracks.Num(), 1))
    {
        const UCurveFloat* Curve = Trip.Source->EventTracks[0].CurveKeys;
        TestTrue(TEXT("Event curve is flagged as an event curve"), Curve && Curve->bIsEventCurve);
    }
    UK2Node_Timeline* Rebuilt = BpirTimelineTracksTest::FindTimelineNode(Trip.RebuiltBlueprint);
    const UEdGraphPin* BeepPin = Rebuilt ? Rebuilt->FindPin(TEXT("Beep")) : nullptr;
    TestTrue(TEXT("Rebuilt node exposes the Beep exec pin"), BeepPin != nullptr);
    TestTrue(TEXT("Rebuilt Beep pin is wired to its handler"), BeepPin && BeepPin->LinkedTo.Num() == 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineSettingsRoundTripTest,
    "PinWright.bpir.timeline_tracks.SettingsRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineSettingsRoundTripTest::RunTest(const FString& Parameters)
{
    BpirTimelineTracksTest::FRoundTrip Trip;
    if (!BpirTimelineTracksTest::RoundTrip(*this, BpirTimelineTracksTest::TimelineProgram(TEXT(
        "length: 3.5, length_mode: last_keyframe, autoplay: true, loop: true, replicated: true, ignore_time_dilation: true, Alpha: float_curve((0, 0), (3.5, 1))")), Trip))
    {
        return false;
    }

    TestEqual(TEXT("length"), Trip.Source->TimelineLength, 3.5f);
    TestEqual(TEXT("length mode"), static_cast<int32>(Trip.Source->LengthMode.GetValue()), static_cast<int32>(TL_LastKeyFrame));
    TestTrue(TEXT("autoplay"), Trip.Source->bAutoPlay != 0);
    TestTrue(TEXT("loop"), Trip.Source->bLoop != 0);
    TestTrue(TEXT("replicated"), Trip.Source->bReplicated != 0);
    TestTrue(TEXT("ignore time dilation"), Trip.Source->bIgnoreTimeDilation != 0);
    UK2Node_Timeline* Node = BpirTimelineTracksTest::FindTimelineNode(Trip.SourceBlueprint);
    TestTrue(TEXT("Node caches autoplay and loop from the template"), Node && Node->bAutoPlay && Node->bLoop);
    TestTrue(TEXT("Decompiled text carries the settings"),
        Trip.Text.Contains(TEXT("length: 3.5, length_mode: last_keyframe, autoplay: true, loop: true, replicated: true, ignore_time_dilation: true")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineReplaceModeKeepsTracksTest,
    "PinWright.bpir.timeline_tracks.ReplaceModeRecompileKeepsTracks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineReplaceModeKeepsTracksTest::RunTest(const FString& Parameters)
{
    // The reference Blueprint is compiled from the same text and never touched again.
    const FString Program = BpirTimelineTracksTest::TimelineProgram(TEXT(
        "loop: true, Alpha: float_curve((0, 0), (1, 1, cubic)), Offset: vector_curve(y((0, 5))), Beep: event_curve((0.5, 0))"));
    UBlueprint* Reference = CreateTransientTestBP(TEXT("TimelineTracksReferenceBP"));
    UBlueprint* BP = CreateTransientTestBP(TEXT("TimelineTracksReplaceBP"));
    if (!TestNotNull(TEXT("Reference Blueprint"), Reference) || !TestNotNull(TEXT("Blueprint"), BP)
        || !BpirTimelineTracksTest::CompileOrReport(*this, Reference, Program) || !BpirTimelineTracksTest::CompileOrReport(*this, BP, Program))
    {
        return false;
    }

    // Replace mode deletes the old node, and UK2Node_Timeline::DestroyNode discards its template.
    const FString Text = BpirTimelineTracksTest::DecompileOrReport(*this, BP);
    if (!BpirTimelineTracksTest::CompileOrReport(*this, BP, Text, EBpirCompileMode::Replace))
    {
        AddInfo(FString::Printf(TEXT("Decompiled text:\n%s"), *Text));
        return false;
    }

    TestEqual(TEXT("Exactly one timeline template after the replace"), BP->Timelines.Num(), 1);
    TestEqual(TEXT("Exactly one timeline node after the replace"), CountNodesOfType<UK2Node_Timeline>(BP), 1);
    UTimelineTemplate* Expected = Reference->FindTimelineTemplateByVariableName(BpirTimelineTracksTest::TimelineName);
    UTimelineTemplate* Actual = BP->FindTimelineTemplateByVariableName(BpirTimelineTracksTest::TimelineName);
    if (TestNotNull(TEXT("Reference template"), Expected) && TestNotNull(TEXT("Replaced template"), Actual))
    {
        TestEqual(TEXT("Three tracks survive the replace"), Actual->GetNumDisplayTracks(), 3);
        BpirTimelineTracksTest::ExpectSameTemplate(*this, Expected, Actual);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineDuplicateNameIsRejectedTest,
    "PinWright.bpir.timeline_tracks.DuplicateNameIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineDuplicateNameIsRejectedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("TimelineTracksDuplicateBP"));
    if (!TestNotNull(TEXT("Blueprint"), BP)
        || !BpirTimelineTracksTest::CompileOrReport(*this, BP, BpirTimelineTracksTest::TimelineProgram(TEXT("Alpha: float_curve((0, 0), (1, 1))"))))
    {
        return false;
    }

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event Other() {\n")
        TEXT("    %t2 = timeline FadeIn(Beta: float_curve((0, 1))) [update -> @u]\n")
        TEXT("\n")
        TEXT("@u:\n")
        TEXT("    call PrintString(InString: \"U\")\n")
        TEXT("}"));
    TestFalse(TEXT("A second timeline with a taken name fails to compile"), Result.bSuccess);
    TestTrue(TEXT("The error says the name already exists"), ErrorsContain(Result.Errors, TEXT("already exists")));

    // The original template must not have received the second timeline's track.
    TestEqual(TEXT("Still one timeline template"), BP->Timelines.Num(), 1);
    UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(BpirTimelineTracksTest::TimelineName);
    if (TestNotNull(TEXT("Original template"), Template))
    {
        TestEqual(TEXT("Original template keeps only its own track"), Template->GetNumDisplayTracks(), 1);
        TestEqual(TEXT("Original track name"), BpirTimelineTracksTest::ViewDisplayTrack(Template, 0).Name.ToString(), TEXT("Alpha"));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTimelineMalformedArgsAreRejectedTest,
    "PinWright.bpir.timeline_tracks.MalformedArgsAreRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirTimelineMalformedArgsAreRejectedTest::RunTest(const FString& Parameters)
{
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("Alpha: float_curve((0))"), TEXT("fields"));
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("Alpha: float_curve((0, x))"), TEXT("must be numbers"));
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("Alpha: float_curve((0, 1, linear, auto, 0, 0))"), TEXT("only to cubic"));
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("loop: maybe"), TEXT("true or false"));
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("Speed: 3"), TEXT("is neither a setting"));
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("Offset: vector_curve(w((0, 1)))"), TEXT("not a channel"));
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("Alpha: float_curve((0, 0)), Alpha: event_curve((1, 0))"), TEXT("appears twice"));
    BpirTimelineTracksTest::ExpectRejected(*this, TEXT("Alpha: float_curve(\"/Game/PinWrightTests/NoSuchTimelineCurve.NoSuchTimelineCurve\")"), TEXT("was not found"));
    return true;
}
