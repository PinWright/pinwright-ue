// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the TRANSFORM half of the keyframe-interpolation gap.
//
// sequencer.add_keyframe (float property tracks) gained `interp`/`tangentMode` and is guarded by
// TestSequencerAddKeyframeInterp.cpp. The sibling verb sequence.add_keyframe — the one that writes
// Transform / Location / Rotation / Scale channels — did NOT: it had no interp parameter at all and
// wrote every key with a default-constructed FMovieSceneDoubleValue, whose InterpMode is RCIM_Cubic
// (Channels/MovieSceneDoubleChannel.h:56-62). Auto tangents on a two-key span are an ease in/ease
// out, so an authored march starts from a dead stop, overshoots the mean rate mid-span, and glides
// to a halt at the last key — on every loop. That is invisible in any single still and is exactly
// the class of defect this test exists to catch.
//
// This drives the PRODUCTION sequence.add_keyframe handler through the real registration list
// (InvokeHandlerWithCapture) three times on ONE binding's Location channels — interp=constant,
// linear, cubic at distinct display frames — then verifies both halves:
//   1. WRITE side: the FMovieSceneDoubleValue stored on Location.X carries the requested InterpMode.
//      Pre-fix every key landed RCIM_Cubic, so the constant/linear assertions FAIL.
//   2. READ side: sequencer.list_sections with includeKeys=true reports a per-key `interp` token
//      that round-trips the authored mode — the readback an agent uses to prove interpolation
//      before shipping.
//
// Fixture is content-free: a real /Game LevelSequence created via the registered sequencer.create
// handler plus an in-code possessable binding. sequence.add_keyframe needs only FindBinding to
// succeed; the transform track/section it authors does not require a bound object instance.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Curves/RichCurve.h"

#include "Tests/TestUtils.h"

namespace
{
    // Distinctly named so anonymous-namespace symbols do not ODR-collide when Unity merges this TU
    // with the sibling interp test: create a real /Game LevelSequence via the registered
    // sequencer.create handler so sequence.add_keyframe's asset load can resolve it.
    ULevelSequence* CreateSequenceAddKeyframeInterpSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_SeqAddKeyframeInterpSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_SeqAddKeyframeInterpProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create — "
                               "the sequence.add_keyframe transform interp repro cannot be exercised."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Location.X channel of the binding's transform section, resolved exactly the way the handler
    // resolves it (FindTrack<UMovieScene3DTransformTrack> then the double-channel proxy, layout
    // 0-2 Location / 3-5 Rotation / 6-8 Scale). nullptr when the handler authored nothing.
    FMovieSceneDoubleChannel* FindTransformLocationXChannel(UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (!MovieScene)
        {
            return nullptr;
        }
        UMovieScene3DTransformTrack* Track =
            MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
        if (!Track)
        {
            return nullptr;
        }
        const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
        if (Sections.Num() == 0)
        {
            return nullptr;
        }
        UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(Sections[0]);
        if (!Section)
        {
            return nullptr;
        }
        TArrayView<FMovieSceneDoubleChannel*> Channels =
            Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
        return Channels.Num() > 0 ? Channels[0] : nullptr;
    }

    // One authoring case: request `Interp` at display frame `Frame`, expect `Expected` on the channel.
    struct FTransformInterpCase
    {
        int32 Frame;
        const TCHAR* Interp;
        ERichCurveInterpMode Expected;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddKeyframeTransformInterpTest,
    "PinWright.Sequencer.SequenceAddKeyframe.TransformInterpModeApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddKeyframeTransformInterpTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }
    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickResolution = MovieScene->GetTickResolution();

    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqAddKeyframeInterpActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    const FTransformInterpCase Cases[] = {
        { 0,  TEXT("constant"), RCIM_Constant },
        { 10, TEXT("linear"),   RCIM_Linear },
        { 20, TEXT("cubic"),    RCIM_Cubic },
    };

    // Author each Location key through the production handler with an explicit interp mode.
    for (const FTransformInterpCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> LocationValue = MakeShared<FJsonObject>();
        LocationValue->SetNumberField(TEXT("x"), static_cast<double>(Case.Frame) * 100.0);
        LocationValue->SetNumberField(TEXT("y"), 0.0);
        LocationValue->SetNumberField(TEXT("z"), 0.0);

        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("path"), FullPath);
        AddPayload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
        AddPayload->SetStringField(TEXT("property"), TEXT("Location"));
        AddPayload->SetNumberField(TEXT("frame"), Case.Frame);
        AddPayload->SetObjectField(TEXT("value"), LocationValue);
        AddPayload->SetStringField(TEXT("interp"), Case.Interp);

        FTestResponseCapture AddCapture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), AddPayload, AddCapture);
        if (!TestTrue(TEXT("sequence.add_keyframe handler is registered and invoked"), bFound))
        {
            return false;
        }
        if (!TestTrue(*FString::Printf(TEXT("sequence.add_keyframe(interp=%s) reported success"), Case.Interp),
                AddCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("sequence.add_keyframe(interp=%s) failed (code=%s msg=%s)"),
                Case.Interp, *AddCapture.ErrorCode, *AddCapture.Message));
            return false;
        }
    }

    // An unrecognized interp must be rejected by name before any mutation, not silently coerced.
    {
        TSharedPtr<FJsonObject> BadValue = MakeShared<FJsonObject>();
        BadValue->SetNumberField(TEXT("x"), 1.0);
        TSharedPtr<FJsonObject> BadPayload = MakeShared<FJsonObject>();
        BadPayload->SetStringField(TEXT("path"), FullPath);
        BadPayload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
        BadPayload->SetStringField(TEXT("property"), TEXT("Location"));
        BadPayload->SetNumberField(TEXT("frame"), 30);
        BadPayload->SetObjectField(TEXT("value"), BadValue);
        BadPayload->SetStringField(TEXT("interp"), TEXT("ease"));

        FTestResponseCapture BadCapture;
        InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), BadPayload, BadCapture);
        TestFalse(TEXT("an unrecognized interp value is rejected"), BadCapture.bSuccess);
    }

    // ---- WRITE side: the stored FMovieSceneDoubleValue.InterpMode must match the request per key ----
    FMovieSceneDoubleChannel* LocationX = FindTransformLocationXChannel(MovieScene, BindingGuid);
    if (!TestNotNull(TEXT("sequence.add_keyframe authored a transform section with a Location.X channel"),
            LocationX))
    {
        return false;
    }
    const TArrayView<const FFrameNumber> Times = LocationX->GetData().GetTimes();
    const auto Values = LocationX->GetData().GetValues();
    if (!TestEqual(TEXT("all three requested keys landed on Location.X"), Times.Num(), 3))
    {
        return false;
    }
    // frame -> stored interp mode, so each case is matched by its tick frame, not by array order.
    TMap<int32, ERichCurveInterpMode> StoredByFrame;
    for (int32 i = 0; i < Times.Num() && i < Values.Num(); ++i)
    {
        StoredByFrame.Add(Times[i].Value, static_cast<ERichCurveInterpMode>(Values[i].InterpMode.GetValue()));
    }
    const auto DisplayFrameToTick = [DisplayRate, TickResolution](int32 Frame)
    {
        return FFrameRate::TransformTime(FFrameTime(FFrameNumber(Frame)), DisplayRate, TickResolution)
            .FloorToFrame().Value;
    };
    for (const FTransformInterpCase& Case : Cases)
    {
        const int32 ExpectedFrame = DisplayFrameToTick(Case.Frame);
        const ERichCurveInterpMode* Stored = StoredByFrame.Find(ExpectedFrame);
        if (!TestNotNull(*FString::Printf(TEXT("a key exists at the tick frame for %s (display frame %d)"),
                Case.Interp, Case.Frame), Stored))
        {
            continue;
        }
        // Pre-fix every key was a default-constructed FMovieSceneDoubleValue (RCIM_Cubic), so the
        // constant and linear assertions fail here.
        TestEqual(*FString::Printf(TEXT("stored InterpMode for the %s key is the requested mode"), Case.Interp),
            static_cast<int32>(*Stored), static_cast<int32>(Case.Expected));
    }

    // ---- READ side: sequencer.list_sections includeKeys=true must surface a per-key `interp` token ----
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), FullPath);
    ListPayload->SetBoolField(TEXT("includeKeys"), true);
    FTestResponseCapture ListCapture;
    if (!TestTrue(TEXT("sequencer.list_sections handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), ListPayload, ListCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.list_sections succeeded"), ListCapture.bSuccess) || !ListCapture.Result.IsValid())
    {
        return false;
    }

    // Build frame -> interp token from the one keyed channel carrying all three keys (Location.X).
    TMap<int32, FString> ReadbackByFrame;
    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    if (ListCapture.Result->TryGetArrayField(TEXT("sections"), Sections))
    {
        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject> SectionObj = SectionValue.IsValid() ? SectionValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* ChannelsArr = nullptr;
            if (!SectionObj.IsValid() || !SectionObj->TryGetArrayField(TEXT("channels"), ChannelsArr))
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& ChannelValue : *ChannelsArr)
            {
                const TSharedPtr<FJsonObject> ChannelObj = ChannelValue.IsValid() ? ChannelValue->AsObject() : nullptr;
                const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
                if (!ChannelObj.IsValid() || !ChannelObj->TryGetArrayField(TEXT("keys"), Keys) || Keys->Num() != 3)
                {
                    continue;
                }
                for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
                {
                    const TSharedPtr<FJsonObject> KeyObj = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
                    if (!KeyObj.IsValid())
                    {
                        continue;
                    }
                    double Frame = 0.0;
                    FString Interp;
                    if (KeyObj->TryGetNumberField(TEXT("frame"), Frame) &&
                        KeyObj->TryGetStringField(TEXT("interp"), Interp))
                    {
                        ReadbackByFrame.Add(static_cast<int32>(Frame), Interp);
                    }
                }
            }
        }
    }

    if (!TestEqual(TEXT("list_sections includeKeys surfaced a per-key interp token for all three keys"),
            ReadbackByFrame.Num(), 3))
    {
        return true; // assertions already recorded the failure
    }
    for (const FTransformInterpCase& Case : Cases)
    {
        const int32 ExpectedFrame = DisplayFrameToTick(Case.Frame);
        const FString* Token = ReadbackByFrame.Find(ExpectedFrame);
        if (!TestNotNull(*FString::Printf(TEXT("readback has an interp token for the %s key"), Case.Interp), Token))
        {
            continue;
        }
        TestEqual(*FString::Printf(TEXT("readback interp token for the %s key round-trips the mode"), Case.Interp),
            *Token, FString(Case.Interp));
    }

    return true;
}

// ============================================================================
// Regression cover for the two defects that lived on the transform branch's raw
// `Channels[n]->GetData().AddKey(...)` writes. Both are behavioural and silent — the verb
// answered `{}` either way — so each test asserts the channel state the fix guarantees:
//   * a re-key REPLACES (key count constant), it does not append a second key at the same tick;
//   * a cubic/auto key gets its tangents SOLVED, not merely stamped RCTM_Auto and left at 0/0.
// Both drive the production handler through the real registration list, exactly as the interp
// test above does, so a revert of the handler edit fails them.
// ============================================================================
namespace
{
    // All nine double channels of the binding's transform section (layout 0-2 Location,
    // 3-5 Rotation, 6-8 Scale), resolved the way the handler resolves them. Empty view when the
    // handler authored nothing. Distinctly named for Unity-merge safety, as above.
    TArrayView<FMovieSceneDoubleChannel*> FindSeqAddKeyframeTransformDoubleChannels(
        UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (!MovieScene)
        {
            return TArrayView<FMovieSceneDoubleChannel*>();
        }
        UMovieScene3DTransformTrack* Track =
            MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
        if (!Track || Track->GetAllSections().Num() == 0)
        {
            return TArrayView<FMovieSceneDoubleChannel*>();
        }
        UMovieScene3DTransformSection* Section =
            Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[0]);
        if (!Section)
        {
            return TArrayView<FMovieSceneDoubleChannel*>();
        }
        return Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    }

    // {location:{x,y,z}, rotation:{roll,pitch,yaw}} — the full-Transform payload shape.
    TSharedPtr<FJsonObject> MakeSeqAddKeyframeTransformValue(
        double X, double Y, double Z, double Roll, double Pitch, double Yaw)
    {
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), X);
        Location->SetNumberField(TEXT("y"), Y);
        Location->SetNumberField(TEXT("z"), Z);

        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("roll"), Roll);
        Rotation->SetNumberField(TEXT("pitch"), Pitch);
        Rotation->SetNumberField(TEXT("yaw"), Yaw);

        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetObjectField(TEXT("location"), Location);
        Value->SetObjectField(TEXT("rotation"), Rotation);
        return Value;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddKeyframeRekeyReplacesTest,
    "PinWright.Sequencer.SequenceAddKeyframe.RekeyReplacesInsteadOfDuplicating",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddKeyframeRekeyReplacesTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }
    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickResolution = MovieScene->GetTickResolution();

    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqAddKeyframeRekeyActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    // property="Transform" on purpose: it is six animated channels driven by one call, which is
    // where a partial fix (some channels routed through UpdateOrAddKey, some not) would hide.
    const auto AuthorTransformKey = [this, &FullPath, &BindingGuid](
        int32 Frame, const TSharedPtr<FJsonObject>& Value, const TCHAR* What) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), FullPath);
        Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
        Payload->SetStringField(TEXT("property"), TEXT("Transform"));
        Payload->SetNumberField(TEXT("frame"), Frame);
        Payload->SetObjectField(TEXT("value"), Value);

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), Payload, Capture))
        {
            AddError(TEXT("sequence.add_keyframe handler is not registered"));
            return false;
        }
        if (!Capture.bSuccess)
        {
            AddError(FString::Printf(TEXT("sequence.add_keyframe(%s, frame %d) failed (code=%s msg=%s)"),
                What, Frame, *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        return true;
    };

    // Three distinct frames, one key each.
    const int32 Frames[] = { 0, 10, 20 };
    for (const int32 Frame : Frames)
    {
        const double F = static_cast<double>(Frame);
        if (!AuthorTransformKey(Frame,
                MakeSeqAddKeyframeTransformValue(F * 100.0, F * 10.0, F * 5.0, 0.0, F, F * 2.0),
                TEXT("initial")))
        {
            return false;
        }
    }

    TArrayView<FMovieSceneDoubleChannel*> Channels =
        FindSeqAddKeyframeTransformDoubleChannels(MovieScene, BindingGuid);
    if (!TestTrue(TEXT("sequence.add_keyframe authored a nine-channel transform section"),
            Channels.Num() >= 9))
    {
        return false;
    }
    // Only Location (0-2) and Rotation (3-5) were written; Scale was never in the payload.
    const int32 AnimatedChannelCount = 6;
    for (int32 ChannelIndex = 0; ChannelIndex < AnimatedChannelCount; ++ChannelIndex)
    {
        if (!TestEqual(*FString::Printf(TEXT("channel %d holds the three authored keys"), ChannelIndex),
                Channels[ChannelIndex]->GetNumKeys(), 3))
        {
            return false;
        }
    }

    // The defect: re-keying frame 10 to fix framing appended a SECOND key at the same tick on
    // every one of the six channels (23 -> 27 in the field report) instead of overwriting.
    // Note the roll value is deliberately UNCHANGED from the initial key — value equality never
    // deduplicated, so the pre-fix duplicate appeared on that channel too.
    const double RekeyX = 999.0;
    if (!AuthorTransformKey(10,
            MakeSeqAddKeyframeTransformValue(RekeyX, 888.0, 777.0, 0.0, 22.0, 33.0), TEXT("re-key")))
    {
        return false;
    }

    // Re-resolve rather than reusing the view taken before the write: the channel objects live on
    // the section and never move, but the proxy that hands out the pointer array can be rebuilt.
    Channels = FindSeqAddKeyframeTransformDoubleChannels(MovieScene, BindingGuid);
    if (!TestTrue(TEXT("the transform section still exposes its nine channels after the re-key"),
            Channels.Num() >= 9))
    {
        return false;
    }

    const int32 RekeyTick =
        FFrameRate::TransformTime(FFrameTime(FFrameNumber(10)), DisplayRate, TickResolution)
            .FloorToFrame().Value;

    for (int32 ChannelIndex = 0; ChannelIndex < AnimatedChannelCount; ++ChannelIndex)
    {
        // Pre-fix this reads 4: GetData().AddKey always inserts, so the re-key appended rather
        // than replacing, leaving two keys at RekeyTick holding different values.
        TestEqual(*FString::Printf(TEXT("re-keying frame 10 leaves channel %d key count unchanged"),
                ChannelIndex),
            Channels[ChannelIndex]->GetNumKeys(), 3);

        int32 KeysAtRekeyTick = 0;
        for (const FFrameNumber Time : Channels[ChannelIndex]->GetData().GetTimes())
        {
            if (Time.Value == RekeyTick)
            {
                ++KeysAtRekeyTick;
            }
        }
        TestEqual(*FString::Printf(TEXT("channel %d carries exactly one key at the re-keyed tick"),
                ChannelIndex),
            KeysAtRekeyTick, 1);
    }

    // The surviving key must hold the SECOND value, not the stale first one — pre-fix the stale
    // value sorted first in the array, so whichever key won evaluation was the wrong one.
    {
        const TArrayView<const FFrameNumber> Times = Channels[0]->GetData().GetTimes();
        const auto Values = Channels[0]->GetData().GetValues();
        bool bFoundRekey = false;
        for (int32 KeyIndex = 0; KeyIndex < Times.Num() && KeyIndex < Values.Num(); ++KeyIndex)
        {
            if (Times[KeyIndex].Value == RekeyTick)
            {
                bFoundRekey = true;
                TestEqual(TEXT("Location.X at the re-keyed tick holds the second value"),
                    Values[KeyIndex].Value, RekeyX);
            }
        }
        TestTrue(TEXT("a key still exists at the re-keyed tick"), bFoundRekey);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddKeyframeCubicAutoTangentsTest,
    "PinWright.Sequencer.SequenceAddKeyframe.CubicAutoKeysGetComputedTangents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddKeyframeCubicAutoTangentsTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqAddKeyframeTangentActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    // A MONOTONIC ramp, not the 0/100/0 hill the ticket sketched: UE's default auto-tangent mode
    // (Sequencer.AutoTangentNew = 2) flattens any key that is not strictly between its neighbours,
    // so a hill's apex is legitimately 0/0 and would make the assertion below pass for the wrong
    // reason — it could not distinguish the fix from the defect. 0 -> 1000 -> 3000 rises
    // throughout, so the middle key has a genuine prev-to-next slope to solve.
    // Routed through property="Location" so the per-axis branch (Channels[ChannelBase + i]) is the
    // one under test here; the re-key test above covers the full-Transform branch.
    const double ZValues[] = { 0.0, 1000.0, 3000.0 };
    const int32 Frames[] = { 0, 50, 100 };
    for (int32 CaseIndex = 0; CaseIndex < 3; ++CaseIndex)
    {
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetNumberField(TEXT("x"), 0.0);
        Value->SetNumberField(TEXT("y"), 0.0);
        Value->SetNumberField(TEXT("z"), ZValues[CaseIndex]);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), FullPath);
        Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
        Payload->SetStringField(TEXT("property"), TEXT("Location"));
        Payload->SetNumberField(TEXT("frame"), Frames[CaseIndex]);
        Payload->SetObjectField(TEXT("value"), Value);
        Payload->SetStringField(TEXT("interp"), TEXT("cubic"));
        Payload->SetStringField(TEXT("tangentMode"), TEXT("auto"));

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("sequence.add_keyframe handler is registered and invoked"),
                InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), Payload, Capture)))
        {
            return false;
        }
        if (!Capture.bSuccess)
        {
            AddError(FString::Printf(TEXT("sequence.add_keyframe(frame %d) failed (code=%s msg=%s)"),
                Frames[CaseIndex], *Capture.ErrorCode, *Capture.Message));
            return false;
        }
    }

    TArrayView<FMovieSceneDoubleChannel*> Channels =
        FindSeqAddKeyframeTransformDoubleChannels(MovieScene, BindingGuid);
    if (!TestTrue(TEXT("sequence.add_keyframe authored a nine-channel transform section"),
            Channels.Num() >= 9))
    {
        return false;
    }
    FMovieSceneDoubleChannel* LocationZ = Channels[2];
    const auto Values = LocationZ->GetData().GetValues();
    if (!TestEqual(TEXT("all three keys landed on Location.Z"), Values.Num(), 3))
    {
        return false;
    }

    // Every key must still report the mode that was requested; the fix must not silently
    // downgrade the intent to make the tangents come out.
    for (int32 KeyIndex = 0; KeyIndex < 3; ++KeyIndex)
    {
        TestEqual(*FString::Printf(TEXT("key %d is cubic"), KeyIndex),
            static_cast<int32>(Values[KeyIndex].InterpMode.GetValue()), static_cast<int32>(RCIM_Cubic));
        TestEqual(*FString::Printf(TEXT("key %d is auto-tangent"), KeyIndex),
            static_cast<int32>(Values[KeyIndex].TangentMode.GetValue()), static_cast<int32>(RCTM_Auto));
    }

    // THE differential assertion. Pre-fix the keys were written through the raw channel-data view,
    // which stores the FMovieSceneDoubleValue verbatim and cannot reach AutoSetTangents() — so
    // RCTM_Auto was an intent nothing acted on and all three keys kept 0/0 tangents, evaluating as
    // a chain of flat-in/flat-out smoothsteps that stops the subject dead at every key.
    const float MiddleArrive = Values[1].Tangent.ArriveTangent;
    const float MiddleLeave = Values[1].Tangent.LeaveTangent;
    TestTrue(*FString::Printf(TEXT("middle cubic/auto key has a non-zero ArriveTangent (got %g)"),
            MiddleArrive),
        FMath::Abs(MiddleArrive) > UE_SMALL_NUMBER);
    TestTrue(*FString::Printf(TEXT("middle cubic/auto key has a non-zero LeaveTangent (got %g)"),
            MiddleLeave),
        FMath::Abs(MiddleLeave) > UE_SMALL_NUMBER);

    // UE's endpoint rule: the first key's and last key's auto tangents are forced flat. This is the
    // behaviour the documented loop-seam decoy-key technique is written against, so pin it here —
    // it holds both before and after the fix and is not the differential.
    TestEqual(TEXT("first cubic/auto key keeps a flat leave tangent"),
        Values[0].Tangent.LeaveTangent, 0.0f);
    TestEqual(TEXT("last cubic/auto key keeps a flat arrive tangent"),
        Values[2].Tangent.ArriveTangent, 0.0f);

    // Readback: list_sections includeKeys must publish the tangent numbers, otherwise this class of
    // fault is invisible from the wire — every other published signal (keyCount, value, interp,
    // tangentMode) reads clean on a curve whose tangents were never computed.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), FullPath);
    ListPayload->SetBoolField(TEXT("includeKeys"), true);
    FTestResponseCapture ListCapture;
    if (!TestTrue(TEXT("sequencer.list_sections handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), ListPayload, ListCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.list_sections succeeded"), ListCapture.bSuccess) ||
        !ListCapture.Result.IsValid())
    {
        return false;
    }

    bool bSawNonZeroTangentInReadback = false;
    bool bSawTangentModeInReadback = false;
    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    if (ListCapture.Result->TryGetArrayField(TEXT("sections"), Sections))
    {
        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject> SectionObj =
                SectionValue.IsValid() ? SectionValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* ChannelsArr = nullptr;
            if (!SectionObj.IsValid() || !SectionObj->TryGetArrayField(TEXT("channels"), ChannelsArr))
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& ChannelValue : *ChannelsArr)
            {
                const TSharedPtr<FJsonObject> ChannelObj =
                    ChannelValue.IsValid() ? ChannelValue->AsObject() : nullptr;
                const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
                if (!ChannelObj.IsValid() || !ChannelObj->TryGetArrayField(TEXT("keys"), Keys))
                {
                    continue;
                }
                for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
                {
                    const TSharedPtr<FJsonObject> KeyObj =
                        KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
                    if (!KeyObj.IsValid())
                    {
                        continue;
                    }
                    FString TangentMode;
                    if (KeyObj->TryGetStringField(TEXT("tangentMode"), TangentMode))
                    {
                        bSawTangentModeInReadback = true;
                    }
                    double Arrive = 0.0;
                    double Leave = 0.0;
                    if (KeyObj->TryGetNumberField(TEXT("arriveTangent"), Arrive) &&
                        KeyObj->TryGetNumberField(TEXT("leaveTangent"), Leave) &&
                        (FMath::Abs(Arrive) > UE_SMALL_NUMBER || FMath::Abs(Leave) > UE_SMALL_NUMBER))
                    {
                        bSawNonZeroTangentInReadback = true;
                    }
                }
            }
        }
    }
    TestTrue(TEXT("list_sections includeKeys emits a per-key tangentMode token"),
        bSawTangentModeInReadback);
    TestTrue(TEXT("list_sections includeKeys emits the solved arriveTangent/leaveTangent numbers"),
        bSawNonZeroTangentInReadback);

    return true;
}

// ============================================================================
// Explicit tangent VALUES, and the batch verb that writes a whole path through the same code.
// ============================================================================
// `tangentMode: "user"` selects the mode that uses CALLER-SUPPLIED tangents; until arriveTangent /
// leaveTangent existed there was no way to supply any, so the mode was decorative. The differential
// pinned below is the loop seam: FMovieSceneDoubleChannel::AutoSetTangents unconditionally forces
// the FIRST key's leave tangent and the LAST key's arrive tangent to 0, so a looping camera move
// authored through this API decelerates to a full stop at the one frame the viewer sees most often.
// User-mode keys are the only ones AutoSetTangents skips, so explicit values are the only thing
// that survives it - which is exactly what the endpoint assertions here measure.
namespace
{
    // Distinctly named for Unity-merge safety, as with the helpers above.
    TSharedPtr<FJsonObject> MakeSeqAddKeyframeXyzValue(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetNumberField(TEXT("x"), X);
        Value->SetNumberField(TEXT("y"), Y);
        Value->SetNumberField(TEXT("z"), Z);
        return Value;
    }

    // The tick-resolution frame sequence.add_keyframe / sequencer.add_keyframes resolve a
    // display-rate frame to, computed the way SequenceHelpers::DisplayFrameToTick computes it.
    int32 SeqAddKeyframeDisplayFrameToTick(const UMovieScene* MovieScene, int32 DisplayFrame)
    {
        return FFrameRate::TransformTime(FFrameTime(FFrameNumber(DisplayFrame)),
                   MovieScene->GetDisplayRate(), MovieScene->GetTickResolution())
            .FloorToFrame()
            .Value;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddKeyframeExplicitTangentsTest,
    "PinWright.Sequencer.SequenceAddKeyframe.ExplicitTangentValuesSurviveUnderUserMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddKeyframeExplicitTangentsTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqAddKeyframeExplicitTangentActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    // Deliberately UNEQUAL, so an implementation that assigns one value to both sides cannot pass,
    // and deliberately per-TICK magnitudes - the unit the channels actually store. A parameter that
    // took uu/s and forgot the tick divide would be wrong by the tick resolution.
    const double ArriveTangent = 0.0125;
    const double LeaveTangent = 0.0375;

    const auto AuthorKey = [&FullPath, &BindingGuid](
        int32 Frame, double Z, const TCHAR* TangentMode, double Arrive, double Leave,
        FTestResponseCapture& OutCapture) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), FullPath);
        Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
        Payload->SetStringField(TEXT("property"), TEXT("Location"));
        Payload->SetNumberField(TEXT("frame"), Frame);
        Payload->SetObjectField(TEXT("value"), MakeSeqAddKeyframeXyzValue(0.0, 0.0, Z));
        Payload->SetStringField(TEXT("interp"), TEXT("cubic"));
        Payload->SetStringField(TEXT("tangentMode"), TangentMode);
        Payload->SetNumberField(TEXT("arriveTangent"), Arrive);
        Payload->SetNumberField(TEXT("leaveTangent"), Leave);
        return InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), Payload, OutCapture);
    };

    const int32 Frames[] = { 0, 50, 100 };
    const double ZValues[] = { 0.0, 1000.0, 3000.0 };
    for (int32 CaseIndex = 0; CaseIndex < 3; ++CaseIndex)
    {
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("sequence.add_keyframe handler is registered and invoked"),
                AuthorKey(Frames[CaseIndex], ZValues[CaseIndex], TEXT("user"),
                    ArriveTangent, LeaveTangent, Capture)))
        {
            return false;
        }
        if (!Capture.bSuccess)
        {
            AddError(FString::Printf(
                TEXT("sequence.add_keyframe(frame %d, tangentMode=user) failed (code=%s msg=%s)"),
                Frames[CaseIndex], *Capture.ErrorCode, *Capture.Message));
            return false;
        }
    }

    TArrayView<FMovieSceneDoubleChannel*> Channels =
        FindSeqAddKeyframeTransformDoubleChannels(MovieScene, BindingGuid);
    if (!TestTrue(TEXT("sequence.add_keyframe authored a nine-channel transform section"),
            Channels.Num() >= 9))
    {
        return false;
    }
    FMovieSceneDoubleChannel* LocationZ = Channels[2];
    const auto Values = LocationZ->GetData().GetValues();
    if (!TestEqual(TEXT("all three keys landed on Location.Z"), Values.Num(), 3))
    {
        return false;
    }

    // THE differential. Every key - INCLUDING key 0 and key 2, the two AutoSetTangents forces flat
    // under `auto` - must hold exactly the numbers the caller asked for. Pre-feature the parameters
    // did not exist, so all six of these read whatever the auto solve produced (0 at the endpoints).
    for (int32 KeyIndex = 0; KeyIndex < 3; ++KeyIndex)
    {
        TestEqual(*FString::Printf(TEXT("key %d keeps the requested user tangent mode"), KeyIndex),
            static_cast<int32>(Values[KeyIndex].TangentMode.GetValue()), static_cast<int32>(RCTM_User));
        TestTrue(*FString::Printf(TEXT("key %d holds the requested ArriveTangent %g (got %g)"),
                KeyIndex, ArriveTangent, Values[KeyIndex].Tangent.ArriveTangent),
            FMath::IsNearlyEqual(static_cast<double>(Values[KeyIndex].Tangent.ArriveTangent),
                ArriveTangent, 1e-6));
        TestTrue(*FString::Printf(TEXT("key %d holds the requested LeaveTangent %g (got %g)"),
                KeyIndex, LeaveTangent, Values[KeyIndex].Tangent.LeaveTangent),
            FMath::IsNearlyEqual(static_cast<double>(Values[KeyIndex].Tangent.LeaveTangent),
                LeaveTangent, 1e-6));
    }

    // Readback: an author who sets a seam velocity has to be able to PROVE it landed, otherwise the
    // only provable route stays python.execute. property="Location" writes X, Y and Z, so all three
    // channels carry the pair - nine keys in total.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), FullPath);
    ListPayload->SetBoolField(TEXT("includeKeys"), true);
    FTestResponseCapture ListCapture;
    if (!TestTrue(TEXT("sequencer.list_sections handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), ListPayload, ListCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.list_sections succeeded"), ListCapture.bSuccess) ||
        !ListCapture.Result.IsValid())
    {
        return false;
    }

    int32 UserTangentKeysInReadback = 0;
    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    if (ListCapture.Result->TryGetArrayField(TEXT("sections"), Sections))
    {
        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject> SectionObj =
                SectionValue.IsValid() ? SectionValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* ChannelsArr = nullptr;
            if (!SectionObj.IsValid() || !SectionObj->TryGetArrayField(TEXT("channels"), ChannelsArr))
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& ChannelValue : *ChannelsArr)
            {
                const TSharedPtr<FJsonObject> ChannelObj =
                    ChannelValue.IsValid() ? ChannelValue->AsObject() : nullptr;
                const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
                if (!ChannelObj.IsValid() || !ChannelObj->TryGetArrayField(TEXT("keys"), Keys))
                {
                    continue;
                }
                for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
                {
                    const TSharedPtr<FJsonObject> KeyObj =
                        KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
                    if (!KeyObj.IsValid())
                    {
                        continue;
                    }
                    FString TangentModeText;
                    double Arrive = 0.0;
                    double Leave = 0.0;
                    if (KeyObj->TryGetStringField(TEXT("tangentMode"), TangentModeText) &&
                        TangentModeText == TEXT("user") &&
                        KeyObj->TryGetNumberField(TEXT("arriveTangent"), Arrive) &&
                        KeyObj->TryGetNumberField(TEXT("leaveTangent"), Leave) &&
                        FMath::IsNearlyEqual(Arrive, ArriveTangent, 1e-6) &&
                        FMath::IsNearlyEqual(Leave, LeaveTangent, 1e-6))
                    {
                        ++UserTangentKeysInReadback;
                    }
                }
            }
        }
    }
    TestEqual(TEXT("list_sections includeKeys reports the authored user tangents on every written key"),
        UserTangentKeysInReadback, 9);

    // The mode/value mismatch must be REFUSED rather than silently accepted: under `auto` the next
    // AutoSetTangents erases the numbers, and from the wire that is indistinguishable from a write
    // that worked. Refusing also has to happen before any mutation.
    {
        FTestResponseCapture RejectCapture;
        if (!TestTrue(TEXT("sequence.add_keyframe handler is registered and invoked"),
                AuthorKey(150, 5000.0, TEXT("auto"), ArriveTangent, LeaveTangent, RejectCapture)))
        {
            return false;
        }
        TestFalse(TEXT("explicit tangents under tangentMode=auto are rejected"),
            RejectCapture.bSuccess);
        TestEqual(TEXT("the rejection is INVALID_ARGUMENT"),
            RejectCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

        TArrayView<FMovieSceneDoubleChannel*> AfterReject =
            FindSeqAddKeyframeTransformDoubleChannels(MovieScene, BindingGuid);
        if (TestTrue(TEXT("the transform section still exposes its nine channels"),
                AfterReject.Num() >= 9))
        {
            TestEqual(TEXT("the rejected call wrote no fourth key"),
                AfterReject[2]->GetNumKeys(), 3);
        }
    }

    return true;
}

// ============================================================================
// sequencer.add_keyframes - the batch verb
// ============================================================================
namespace
{
    // One {frame, value:{location, rotation}} entry of a batch, optionally carrying a per-key
    // interp override. Distinctly named for Unity-merge safety, as above.
    TSharedPtr<FJsonValue> MakeSeqAddKeyframesBatchEntry(int32 Frame, double X, double Y, double Z,
                                                         double Roll, double Pitch, double Yaw,
                                                         const TCHAR* InterpOverride)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("frame"), Frame);
        Entry->SetObjectField(TEXT("value"),
            MakeSeqAddKeyframeTransformValue(X, Y, Z, Roll, Pitch, Yaw));
        if (InterpOverride)
        {
            Entry->SetStringField(TEXT("interp"), InterpOverride);
        }
        return MakeShared<FJsonValueObject>(Entry);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddKeyframesBatchTest,
    "PinWright.Sequencer.SequencerAddKeyframes.BatchWritesEveryKeyWithPerKeyOverrides",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddKeyframesBatchTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqAddKeyframesBatchActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    // Three keys, one call. The middle key overrides the batch interp so a batch that ignores
    // per-key fields (or one that applies a per-key field to the whole batch) fails here.
    const int32 Frames[] = { 0, 30, 60 };
    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Add(MakeSeqAddKeyframesBatchEntry(Frames[0], 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, nullptr));
    Keys.Add(MakeSeqAddKeyframesBatchEntry(Frames[1], 300.0, 30.0, 15.0, 0.0, 5.0, 10.0, TEXT("constant")));
    Keys.Add(MakeSeqAddKeyframesBatchEntry(Frames[2], 600.0, 60.0, 30.0, 0.0, 10.0, 20.0, nullptr));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), FullPath);
    Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
    Payload->SetStringField(TEXT("property"), TEXT("Transform"));
    Payload->SetStringField(TEXT("interp"), TEXT("linear"));
    Payload->SetArrayField(TEXT("keys"), Keys);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.add_keyframes handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_keyframes"), Payload, Capture)))
    {
        return false;
    }
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("sequencer.add_keyframes failed (code=%s msg=%s)"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    // The echo is the whole point of the batch shape: 23 single-key calls returning {} force a
    // separate list_sections readback to learn anything, and at that size the readback is only
    // affordable once, at the end, where it cannot say which call failed.
    double Written = 0.0;
    TestTrue(TEXT("the response carries a written count"),
        Capture.Result->TryGetNumberField(TEXT("written"), Written));
    TestEqual(TEXT("written reports all three keys"), static_cast<int32>(Written), 3);

    const TArray<TSharedPtr<FJsonValue>>* EchoKeys = nullptr;
    if (TestTrue(TEXT("the response echoes a keys[] array"),
            Capture.Result->TryGetArrayField(TEXT("keys"), EchoKeys)) &&
        TestEqual(TEXT("the echo has one entry per written key"), EchoKeys->Num(), 3))
    {
        for (int32 KeyIndex = 0; KeyIndex < 3; ++KeyIndex)
        {
            const TSharedPtr<FJsonValue>& RowValue = (*EchoKeys)[KeyIndex];
            const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
            if (!Row.IsValid())
            {
                AddError(FString::Printf(TEXT("keys[%d] of the echo is not an object"), KeyIndex));
                continue;
            }
            double EchoFrame = -1.0;
            double EchoTick = -1.0;
            Row->TryGetNumberField(TEXT("frame"), EchoFrame);
            Row->TryGetNumberField(TEXT("tickFrame"), EchoTick);
            TestEqual(*FString::Printf(TEXT("echo key %d reports its display frame"), KeyIndex),
                static_cast<int32>(EchoFrame), Frames[KeyIndex]);
            TestEqual(*FString::Printf(TEXT("echo key %d reports its resolved tick frame"), KeyIndex),
                static_cast<int32>(EchoTick),
                SeqAddKeyframeDisplayFrameToTick(MovieScene, Frames[KeyIndex]));
        }
    }

    // The section must span the keys: FindOrAddSection(0) creates it collapsed to [0,0], so a batch
    // that forgets to expand leaves a path that plays no time.
    const TSharedPtr<FJsonObject>* RangeObj = nullptr;
    if (TestTrue(TEXT("the response reports the resulting section range"),
            Capture.Result->TryGetObjectField(TEXT("sectionRange"), RangeObj)) && RangeObj)
    {
        double RangeEnd = 0.0;
        (*RangeObj)->TryGetNumberField(TEXT("end"), RangeEnd);
        TestTrue(TEXT("the section range spans the last written key"),
            static_cast<int32>(RangeEnd) >= SeqAddKeyframeDisplayFrameToTick(MovieScene, Frames[2]));
    }

    TArrayView<FMovieSceneDoubleChannel*> Channels =
        FindSeqAddKeyframeTransformDoubleChannels(MovieScene, BindingGuid);
    if (!TestTrue(TEXT("the batch authored a nine-channel transform section"), Channels.Num() >= 9))
    {
        return false;
    }
    // Location (0-2) and Rotation (3-5) were in the payload; Scale (6-8) was not and must stay bare.
    for (int32 ChannelIndex = 0; ChannelIndex < 6; ++ChannelIndex)
    {
        TestEqual(*FString::Printf(TEXT("channel %d holds all three batched keys"), ChannelIndex),
            Channels[ChannelIndex]->GetNumKeys(), 3);
    }
    for (int32 ChannelIndex = 6; ChannelIndex < 9; ++ChannelIndex)
    {
        TestEqual(*FString::Printf(TEXT("unaddressed scale channel %d stays empty"), ChannelIndex),
            Channels[ChannelIndex]->GetNumKeys(), 0);
    }

    const auto LocationXValues = Channels[0]->GetData().GetValues();
    if (TestEqual(TEXT("Location.X carries the three batched values"), LocationXValues.Num(), 3))
    {
        const double ExpectedX[] = { 0.0, 300.0, 600.0 };
        // Batch default `linear` on keys 0 and 2, per-key `constant` override on key 1.
        const ERichCurveInterpMode ExpectedInterp[] = { RCIM_Linear, RCIM_Constant, RCIM_Linear };
        for (int32 KeyIndex = 0; KeyIndex < 3; ++KeyIndex)
        {
            TestEqual(*FString::Printf(TEXT("Location.X key %d holds its authored value"), KeyIndex),
                LocationXValues[KeyIndex].Value, ExpectedX[KeyIndex]);
            TestEqual(*FString::Printf(TEXT("Location.X key %d carries the expected interp"), KeyIndex),
                static_cast<int32>(LocationXValues[KeyIndex].InterpMode.GetValue()),
                static_cast<int32>(ExpectedInterp[KeyIndex]));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddKeyframesAllOrNothingTest,
    "PinWright.Sequencer.SequencerAddKeyframes.InvalidKeyRejectsTheWholeBatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddKeyframesAllOrNothingTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqAddKeyframesAtomicActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    // Two good keys followed by one with no `frame`. The point of the batch shape is that a bad
    // entry at the END cannot leave the good ones behind: a partial write produces a valid,
    // loadable, silently half-authored sequence that no readback flags as wrong.
    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Add(MakeSeqAddKeyframesBatchEntry(0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, nullptr));
    Keys.Add(MakeSeqAddKeyframesBatchEntry(30, 300.0, 30.0, 15.0, 0.0, 5.0, 10.0, nullptr));
    {
        TSharedPtr<FJsonObject> BadEntry = MakeShared<FJsonObject>();
        BadEntry->SetObjectField(TEXT("value"),
            MakeSeqAddKeyframeTransformValue(600.0, 60.0, 30.0, 0.0, 10.0, 20.0));
        Keys.Add(MakeShared<FJsonValueObject>(BadEntry));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), FullPath);
    Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
    Payload->SetStringField(TEXT("property"), TEXT("Transform"));
    Payload->SetArrayField(TEXT("keys"), Keys);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.add_keyframes handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_keyframes"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("a batch carrying an invalid key is rejected"), Capture.bSuccess);
    TestEqual(TEXT("the rejection is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(*FString::Printf(TEXT("the rejection names the offending index (got '%s')"), *Capture.Message),
        Capture.Message.Contains(TEXT("keys[2]")));

    // Nothing may have landed - not the two good keys, and not even the transform track the write
    // would have created on its way to them.
    TArrayView<FMovieSceneDoubleChannel*> Channels =
        FindSeqAddKeyframeTransformDoubleChannels(MovieScene, BindingGuid);
    TestEqual(TEXT("the rejected batch created no transform track at all"), Channels.Num(), 0);

    return true;
}

// ============================================================================
// sequencer.measure_motion - the JUDGE side of everything above.
// ============================================================================
// The write verbs and sequencer.list_sections {includeKeys:true} between them expose every
// AUTHORED number on a transform track, and a camera can pass all of them and still read as
// jagged, because none of them reports a DERIVATIVE. These two tests pin the two halves that
// matter and are exactly the pair the other verbs cannot separate:
//
//   1. The numbers are RIGHT, to the analytic value, including the per-TICK -> per-SECOND
//      conversion. Tangents are stored as curve value per tick; every reported quantity is per
//      second. A missing or doubled tick-resolution factor is a 24000x error at the default
//      resolution, and there is nowhere else in this verb it could hide.
//   2. A loop seam that STOPS DEAD is caught. Both tests author the same monotonic ramp and
//      differ only in tangent mode, so the seam verdict flips purely on the thing being measured.
//      This is why the seam check cannot be a bare mismatch number: under `auto` the mismatch is
//      exactly ZERO - the move halts on both sides - and a mismatch-only check would call the
//      stopping camera a perfect C1 join.
namespace
{
    // {x, y, z} all equal, so the path is the straight line (V,V,V). Two properties come with
    // that and both are load-bearing: the move has ONE exact analytic speed with zero curvature,
    // and the single tangent value sequencer.add_keyframes stamps on all three channels is the
    // correct slope for each of them. A payload that ramped only X would hand Y and Z that same
    // non-zero tangent on a constant value and bend the path into an S.
    TSharedPtr<FJsonValue> MakeSeqMeasureMotionRampKey(int32 Frame, double Value)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("frame"), Frame);
        Entry->SetObjectField(TEXT("value"), MakeSeqAddKeyframeXyzValue(Value, Value, Value));
        return MakeShared<FJsonValueObject>(Entry);
    }

    TSharedPtr<FJsonObject> FindSeqMeasureMotionCheck(const TSharedPtr<FJsonObject>& Result,
        const TCHAR* CheckName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("checks"), Checks) || !Checks)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Checks)
        {
            const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            FString Name;
            if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), Name) && Name == CheckName)
            {
                return Obj;
            }
        }
        return nullptr;
    }

    // Author the shared fixture: three Location keys at display frames 0/30/60 on a straight line
    // whose slope is exactly `Slope` curve units per TICK, written with the requested tangent
    // mode. Returns false having already emitted the error.
    bool AuthorSeqMeasureMotionRamp(FAutomationTestBase& Test, const FString& FullPath,
        const FGuid& BindingGuid, const UMovieScene* MovieScene, double Slope,
        const TCHAR* TangentMode, bool bExplicitTangents)
    {
        const int32 Frames[3] = { 0, 30, 60 };
        const int32 BaseTick = SeqAddKeyframeDisplayFrameToTick(MovieScene, Frames[0]);

        TArray<TSharedPtr<FJsonValue>> Keys;
        for (const int32 Frame : Frames)
        {
            const double Tick =
                static_cast<double>(SeqAddKeyframeDisplayFrameToTick(MovieScene, Frame) - BaseTick);
            Keys.Add(MakeSeqMeasureMotionRampKey(Frame, Slope * Tick));
        }

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), FullPath);
        Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
        Payload->SetStringField(TEXT("property"), TEXT("Location"));
        Payload->SetStringField(TEXT("interp"), TEXT("cubic"));
        Payload->SetStringField(TEXT("tangentMode"), TangentMode);
        if (bExplicitTangents)
        {
            Payload->SetNumberField(TEXT("arriveTangent"), Slope);
            Payload->SetNumberField(TEXT("leaveTangent"), Slope);
        }
        Payload->SetArrayField(TEXT("keys"), Keys);

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("sequencer.add_keyframes"), Payload, Capture))
        {
            Test.AddError(TEXT("sequencer.add_keyframes handler is not registered"));
            return false;
        }
        if (!Capture.bSuccess)
        {
            Test.AddError(FString::Printf(
                TEXT("sequencer.add_keyframes(tangentMode=%s) failed (code=%s msg=%s)"),
                TangentMode, *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        return true;
    }

    // Drive the production sequencer.measure_motion handler through the real registration list.
    bool MeasureSeqMotion(FAutomationTestBase& Test, const FString& FullPath,
        const FGuid& BindingGuid, FTestResponseCapture& OutCapture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), FullPath);
        Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
        Payload->SetBoolField(TEXT("looping"), true);

        if (!InvokeHandlerWithCapture(TEXT("sequencer.measure_motion"), Payload, OutCapture))
        {
            Test.AddError(TEXT("sequencer.measure_motion handler is not registered"));
            return false;
        }
        if (!OutCapture.bSuccess || !OutCapture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("sequencer.measure_motion failed (code=%s msg=%s)"),
                *OutCapture.ErrorCode, *OutCapture.Message));
            return false;
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerMeasureMotionAnalyticRampTest,
    "PinWright.Sequencer.SequencerMeasureMotion.RampDerivativesMatchTheAnalyticValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerMeasureMotionAnalyticRampTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }
    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqMeasureMotionRampActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    // 6000 units per axis over 60 display frames, expressed as the per-TICK slope the channel
    // actually stores. Rounded through float first because FMovieSceneTangentData holds tangents
    // as float32: taking the double round-trip here keeps the authored curve an EXACT straight
    // line, so any acceleration the verb reports is the verb's error and not the fixture's.
    const int32 StartTick = SeqAddKeyframeDisplayFrameToTick(MovieScene, 0);
    const int32 EndTick = SeqAddKeyframeDisplayFrameToTick(MovieScene, 60);
    const double TickSpan = static_cast<double>(EndTick - StartTick);
    if (!TestTrue(TEXT("the 60-frame span resolves to a non-zero tick span"), TickSpan > 0.0))
    {
        return false;
    }
    const double Slope = static_cast<double>(static_cast<float>(6000.0 / TickSpan));
    const double TicksPerSecond = MovieScene->GetTickResolution().AsDecimal();

    // THE analytic value, and THE units assertion: a slope stored per tick becomes a speed per
    // second by multiplying by ticksPerSecond, and the path runs along (1,1,1) so its speed is
    // sqrt(3) times one axis's.
    const double ExpectedSpeed = Slope * TicksPerSecond * FMath::Sqrt(3.0);

    if (!AuthorSeqMeasureMotionRamp(*this, FullPath, BindingGuid, MovieScene, Slope,
            TEXT("user"), /*bExplicitTangents=*/true))
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!MeasureSeqMotion(*this, FullPath, BindingGuid, Capture))
    {
        return false;
    }

    // ---- speed: constant, and equal to the analytic value on every statistic ----
    const TSharedPtr<FJsonObject> Speed = FindSeqMeasureMotionCheck(Capture.Result, TEXT("speed"));
    if (!TestValid(TEXT("the response carries a speed check"), Speed))
    {
        return false;
    }
    FString SpeedStatus;
    Speed->TryGetStringField(TEXT("status"), SpeedStatus);
    TestEqual(TEXT("speed is reported, never gated - it is a profile, not a score"),
        SpeedStatus, FString(TEXT("reported")));

    static const TCHAR* const SpeedFields[3] = { TEXT("min"), TEXT("max"), TEXT("mean") };
    for (const TCHAR* Field : SpeedFields)
    {
        double Value = 0.0;
        if (!TestTrue(*FString::Printf(TEXT("speed.%s is present"), Field),
                Speed->TryGetNumberField(Field, Value)))
        {
            continue;
        }
        // Relative tolerance: the fixture is exact, so anything past float round-off means the
        // tick-to-second conversion or the Bezier basis is wrong.
        TestTrue(*FString::Printf(TEXT("speed.%s is the analytic %.6f uu/s (got %.6f)"),
                Field, ExpectedSpeed, Value),
            FMath::Abs(Value - ExpectedSpeed) <= ExpectedSpeed * 1.0e-6);
    }

    // ---- acceleration and jerk: a straight line at constant speed has neither ----
    const TSharedPtr<FJsonObject> Acceleration =
        FindSeqMeasureMotionCheck(Capture.Result, TEXT("acceleration"));
    if (TestValid(TEXT("the response carries an acceleration check"), Acceleration))
    {
        double MaxStep = -1.0;
        TestTrue(TEXT("acceleration.maxStep is present"),
            Acceleration->TryGetNumberField(TEXT("maxStep"), MaxStep));
        TestTrue(*FString::Printf(
                TEXT("the interior key of an exact straight line has no acceleration step (got %g)"),
                MaxStep),
            MaxStep >= 0.0 && MaxStep < 1.0e-3);
    }
    const TSharedPtr<FJsonObject> Jerk = FindSeqMeasureMotionCheck(Capture.Result, TEXT("jerk"));
    if (TestValid(TEXT("the response carries a jerk check"), Jerk))
    {
        double MaxJerk = -1.0;
        TestTrue(TEXT("jerk.max is present"), Jerk->TryGetNumberField(TEXT("max"), MaxJerk));
        TestTrue(*FString::Printf(TEXT("an exact straight line carries no jerk (got %g)"), MaxJerk),
            MaxJerk >= 0.0 && MaxJerk < 1.0e-3);

        // The duration column is not decoration: jerk scales as 1/h^3, so an outlier on a short
        // segment is a re-timing problem rather than a re-shaping one, and a caller cannot tell
        // the two apart without it.
        const TArray<TSharedPtr<FJsonValue>>* Segments = nullptr;
        if (TestTrue(TEXT("jerk reports its segments"),
                Jerk->TryGetArrayField(TEXT("segments"), Segments)) && Segments)
        {
            TestEqual(TEXT("three keys make two segments"), Segments->Num(), 2);
            for (const TSharedPtr<FJsonValue>& SegmentValue : *Segments)
            {
                const TSharedPtr<FJsonObject> Segment =
                    SegmentValue.IsValid() ? SegmentValue->AsObject() : nullptr;
                double Duration = -1.0;
                if (Segment.IsValid())
                {
                    Segment->TryGetNumberField(TEXT("durationSeconds"), Duration);
                }
                TestTrue(TEXT("every jerk segment reports its duration alongside the magnitude"),
                    Duration > 0.0);
            }
        }
    }

    // ---- curvature: a straight path has no corner, and that is a measurement, not a gap ----
    const TSharedPtr<FJsonObject> Curvature =
        FindSeqMeasureMotionCheck(Capture.Result, TEXT("curvature"));
    if (TestValid(TEXT("the response carries a curvature check"), Curvature))
    {
        bool bStraight = false;
        Curvature->TryGetBoolField(TEXT("straight"), bStraight);
        double MinRadius = 0.0;
        const bool bHasRadius = Curvature->TryGetNumberField(TEXT("minTurnRadius"), MinRadius);
        TestTrue(TEXT("the (1,1,1) line is reported straight, or with a radius orders of magnitude "
                      "past its own length"),
            bStraight || (bHasRadius && MinRadius > 1.0e6));
    }

    // ---- loopSeam: matching non-zero velocities on both sides is a genuine C1 join ----
    const TSharedPtr<FJsonObject> Seam = FindSeqMeasureMotionCheck(Capture.Result, TEXT("loopSeam"));
    if (TestValid(TEXT("the response carries a loopSeam check"), Seam))
    {
        FString SeamStatus;
        Seam->TryGetStringField(TEXT("status"), SeamStatus);
        TestEqual(TEXT("a seam whose one-sided velocities match passes"),
            SeamStatus, FString(TEXT("pass")));

        bool bStopsDead = true;
        Seam->TryGetBoolField(TEXT("stopsDead"), bStopsDead);
        TestFalse(TEXT("explicit user tangents keep the move alive across the seam"), bStopsDead);

        double ArriveSpeed = 0.0;
        double LeaveSpeed = 0.0;
        Seam->TryGetNumberField(TEXT("arriveSpeed"), ArriveSpeed);
        Seam->TryGetNumberField(TEXT("leaveSpeed"), LeaveSpeed);
        TestTrue(*FString::Printf(TEXT("seam arriveSpeed is the analytic %.6f uu/s (got %.6f)"),
                ExpectedSpeed, ArriveSpeed),
            FMath::Abs(ArriveSpeed - ExpectedSpeed) <= ExpectedSpeed * 1.0e-6);
        TestTrue(*FString::Printf(TEXT("seam leaveSpeed is the analytic %.6f uu/s (got %.6f)"),
                ExpectedSpeed, LeaveSpeed),
            FMath::Abs(LeaveSpeed - ExpectedSpeed) <= ExpectedSpeed * 1.0e-6);
    }

    // ---- The response must state its units and its sampling basis, or the numbers are unusable ----
    const TSharedPtr<FJsonObject>* Units = nullptr;
    if (TestTrue(TEXT("the response declares its units"),
            Capture.Result->TryGetObjectField(TEXT("units"), Units)) && Units)
    {
        FString SpeedUnit;
        (*Units)->TryGetStringField(TEXT("speed"), SpeedUnit);
        TestEqual(TEXT("speed is reported per second, not per tick"), SpeedUnit, FString(TEXT("uu/s")));
    }
    double ReportedTicksPerSecond = 0.0;
    Capture.Result->TryGetNumberField(TEXT("ticksPerSecond"), ReportedTicksPerSecond);
    TestTrue(TEXT("the response echoes the tick resolution the conversion used"),
        FMath::IsNearlyEqual(ReportedTicksPerSecond, TicksPerSecond, 1.0e-6));

    bool bPass = false;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestTrue(TEXT("nothing failed and nothing was unmeasurable, so the verdict is a pass"), bPass);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerMeasureMotionAutoTangentSeamTest,
    "PinWright.Sequencer.SequencerMeasureMotion.AutoTangentSeamIsReportedAsAStop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerMeasureMotionAutoTangentSeamTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateSequenceAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }
    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_SeqMeasureMotionSeamActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    // The SAME monotonic ramp as the test above - deliberately monotonic, because UE's default
    // auto-tangent mode flattens any key not strictly between its neighbours, so a hill's apex
    // would be legitimately 0/0 and could not separate the fix from the defect. Only the tangent
    // mode differs, so the verdict below flips purely on the thing being measured.
    const int32 StartTick = SeqAddKeyframeDisplayFrameToTick(MovieScene, 0);
    const int32 EndTick = SeqAddKeyframeDisplayFrameToTick(MovieScene, 60);
    const double TickSpan = static_cast<double>(EndTick - StartTick);
    if (!TestTrue(TEXT("the 60-frame span resolves to a non-zero tick span"), TickSpan > 0.0))
    {
        return false;
    }
    const double Slope = static_cast<double>(static_cast<float>(6000.0 / TickSpan));

    if (!AuthorSeqMeasureMotionRamp(*this, FullPath, BindingGuid, MovieScene, Slope,
            TEXT("auto"), /*bExplicitTangents=*/false))
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!MeasureSeqMotion(*this, FullPath, BindingGuid, Capture))
    {
        return false;
    }

    // THE differential. AutoSetTangents unconditionally forces the FIRST key's leave tangent and
    // the LAST key's arrive tangent to zero, so a looping move authored this way halts at the one
    // frame the viewer sees most often. Note what the mismatch reads: ZERO, because the move is
    // stopped on BOTH sides. A seam check built on the mismatch alone calls this a perfect join,
    // which is precisely how a stopping camera shipped here before.
    const TSharedPtr<FJsonObject> Seam = FindSeqMeasureMotionCheck(Capture.Result, TEXT("loopSeam"));
    if (!TestValid(TEXT("the response carries a loopSeam check"), Seam))
    {
        return false;
    }

    double ArriveSpeed = -1.0;
    double LeaveSpeed = -1.0;
    Seam->TryGetNumberField(TEXT("arriveSpeed"), ArriveSpeed);
    Seam->TryGetNumberField(TEXT("leaveSpeed"), LeaveSpeed);
    TestTrue(*FString::Printf(TEXT("the auto-tangent seam arrives at a standstill (got %g uu/s)"),
            ArriveSpeed), ArriveSpeed >= 0.0 && ArriveSpeed < 1.0e-6);
    TestTrue(*FString::Printf(TEXT("the auto-tangent seam leaves at a standstill (got %g uu/s)"),
            LeaveSpeed), LeaveSpeed >= 0.0 && LeaveSpeed < 1.0e-6);

    double Mismatch = -1.0;
    Seam->TryGetNumberField(TEXT("mismatch"), Mismatch);
    TestTrue(*FString::Printf(
            TEXT("the mismatch is zero and proves nothing on its own (got %g)"), Mismatch),
        Mismatch >= 0.0 && Mismatch < 1.0e-6);

    bool bStopsDead = false;
    Seam->TryGetBoolField(TEXT("stopsDead"), bStopsDead);
    TestTrue(TEXT("the seam is reported as a full stop, not as a clean C1 join"), bStopsDead);

    FString SeamStatus;
    Seam->TryGetStringField(TEXT("status"), SeamStatus);
    TestEqual(TEXT("a stopped seam FAILS - loopSeam is the one genuine pass/fail here"),
        SeamStatus, FString(TEXT("fail")));

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a failed check drives the top-level verdict false"), bPass);

    // The speed profile has to show the same thing from the other direction: the move is at rest
    // at both ends, so its minimum speed is zero while its maximum is not.
    const TSharedPtr<FJsonObject> Speed = FindSeqMeasureMotionCheck(Capture.Result, TEXT("speed"));
    if (TestValid(TEXT("the response carries a speed check"), Speed))
    {
        double MinSpeed = -1.0;
        double MaxSpeed = -1.0;
        Speed->TryGetNumberField(TEXT("min"), MinSpeed);
        Speed->TryGetNumberField(TEXT("max"), MaxSpeed);
        TestTrue(*FString::Printf(TEXT("the flattened endpoints put the minimum speed at zero "
                "(got %g)"), MinSpeed), MinSpeed >= 0.0 && MinSpeed < 1.0e-6);
        TestTrue(*FString::Printf(TEXT("the interior of the move is not at rest (got %g)"), MaxSpeed),
            MaxSpeed > 1.0);
    }

    return true;
}
