// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for F-sequencer-controlrig-track.
//
// PinWright's sequencer namespace has ZERO Control Rig support: grep
// ControlRig|controlrig in Handlers/Sequencer/ = zero, and the controlrig namespace
// is CRIR asset round-trip only (controlrig.compile_crir / decompile_crir) with no
// sequencer integration. There is no way to add a Control Rig track to a binding,
// list/key CR controls, or read control values back — the entry point to the whole
// CR-in-Sequencer cinematics workflow (UE 5.8 ships a 72-tool suite over
// ControlRigSequencerLibrary; PinWright has none of it).
//
// This test asserts the ticket's proposed capability: the four CR-in-Sequencer RPCs
// must be registered, and (once they exist) adding a CR track to a skeletal-mesh
// binding, keying a control at two frames, and reading it back must round-trip the
// keyed value within tolerance — the ticket's acceptance criterion. It routes every
// step through the production registration list (InvokeHandlerWithCapture), never a
// re-implemented local evaluator.
//
// Differential property: pre-fix none of sequencer.add_controlrig_track /
// list_controls / key_controls / get_control_value is registered, so the
// registration assertions FAIL, reproducing the capability gap exactly. Once an
// implementer wires CR track add + control keying + evaluated readback, the
// assertions flip green.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Editor.h"
#include "Misc/ScopeExit.h"
#include "Utils/AssetUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Distinctly named to avoid anonymous-namespace ODR collision with sibling
    // sequencer test .cpp files when Unity merges TUs (mirrors
    // CreateEvalReadbackSequence in TestSequencerEvaluatedReadback.cpp): create a
    // real /Game LevelSequence via the registered sequencer.create handler so a
    // CR-track handler's asset load can resolve it. Empty path + nullptr on failure.
    ULevelSequence* CreateControlRigTrackSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_CRTrackSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_CRTrackProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("sequence-factory-unavailable"),
                TEXT("Could not create a probe sequence (factory unavailable in this "
                     "host); the registration assertions above still stand"));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // The Control Rig section the RPCs key into, resolved exactly as the handler's own
    // ResolveControlRigSection does (section-to-key first, else the track's first section).
    UMovieSceneControlRigParameterSection* CRTrackFindKeyedSection(
        UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (!MovieScene)
        {
            return nullptr;
        }
        for (UMovieSceneTrack* Found : MovieScene->FindTracks(
                 UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid, NAME_None))
        {
            UMovieSceneControlRigParameterTrack* CRTrack =
                Cast<UMovieSceneControlRigParameterTrack>(Found);
            if (!CRTrack)
            {
                continue;
            }
            if (UMovieSceneControlRigParameterSection* ToKey =
                    Cast<UMovieSceneControlRigParameterSection>(CRTrack->GetSectionToKey()))
            {
                return ToKey;
            }
            if (CRTrack->GetAllSections().Num() > 0)
            {
                return Cast<UMovieSceneControlRigParameterSection>(CRTrack->GetAllSections()[0]);
            }
        }
        return nullptr;
    }

    // The section's float channels for one control, ordered by the ENGINE's own per-control
    // channel index (UE::MovieScene::FControlRigChannelMetaData::GetChannelIndex) -- which is
    // the order key_controls documents as [TX,TY,TZ,RX,RY,RZ,SX,SY,SZ].
    //
    // Deliberately NOT the handler's file-static ChannelsForControl. key_controls (write) and
    // get_control_value (read) both order channels through that one accessor, so inverting its
    // sort comparator reverses write and read together and every per-channel value still
    // round-trips to its own slot. Anchoring one end of the mapping to engine metadata is what
    // makes such an inversion observable.
    TArray<FMovieSceneFloatChannel*> CRTrackControlChannelsByEngineIndex(
        UMovieSceneControlRigParameterSection* Section, FName ControlName)
    {
        TArray<TPair<int32, FMovieSceneFloatChannel*>> Matched;
        if (Section)
        {
            FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
            for (FMovieSceneFloatChannel* Channel : Proxy.GetChannels<FMovieSceneFloatChannel>())
            {
                if (!Channel)
                {
                    continue;
                }
                UE::MovieScene::FControlRigChannelMetaData Meta = Section->GetChannelMetaData(Channel);
                if (static_cast<bool>(Meta) && Meta.GetControlName() == ControlName)
                {
                    Matched.Add(TPair<int32, FMovieSceneFloatChannel*>(Meta.GetChannelIndex(), Channel));
                }
            }
        }
        Matched.Sort([](const TPair<int32, FMovieSceneFloatChannel*>& A,
                         const TPair<int32, FMovieSceneFloatChannel*>& B) { return A.Key < B.Key; });
        TArray<FMovieSceneFloatChannel*> Result;
        Result.Reserve(Matched.Num());
        for (const TPair<int32, FMovieSceneFloatChannel*>& Pair : Matched)
        {
            Result.Add(Pair.Value);
        }
        return Result;
    }

    // Display-rate frame -> tick-resolution frame, re-derived from the MovieScene's OWN rates
    // (mirrors PinWrightControlRigSequencer::DisplayFrameToTick, which is a file-internal
    // static in ControlRigSequencerHandler.cpp and so cannot be called from a test TU).
    FFrameNumber CRTrackExpectedTick(const UMovieScene* MovieScene, int32 DisplayFrame)
    {
        return FFrameRate::TransformTime(FFrameTime(FFrameNumber(DisplayFrame)),
            MovieScene->GetDisplayRate(), MovieScene->GetTickResolution())
            .RoundToFrame();
    }
#endif // UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerControlRigTrackRoundTripTest,
    "PinWright.Sequencer.ControlRigTrack.AddKeyReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerControlRigTrackRoundTripTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    // Core capability-gap reproduction: the four CR-in-Sequencer RPCs the ticket
    // proposes must be registered. Pre-fix none of them exist, so these assertions
    // fail — the exact defect the ticket reports (zero CR support in the sequencer
    // namespace).
    TestTrue(TEXT("sequencer.add_controlrig_track handler registered"),
        IsHandlerRegistered(TEXT("sequencer.add_controlrig_track")));
    TestTrue(TEXT("sequencer.list_controls handler registered"),
        IsHandlerRegistered(TEXT("sequencer.list_controls")));
    TestTrue(TEXT("sequencer.key_controls handler registered"),
        IsHandlerRegistered(TEXT("sequencer.key_controls")));
    TestTrue(TEXT("sequencer.get_control_value handler registered"),
        IsHandlerRegistered(TEXT("sequencer.get_control_value")));

    // Host-dependent fixture gate: the acceptance round-trip below binds a Lyra
    // mannequin skeletal mesh so the FK Control Rig can generate per-bone controls,
    // and many hosts do not ship mannequin content. Skip (after the
    // registration assertions above, which are host-independent) with the
    // audit-greppable FIXTURE-SKIP note; a candidate package that exists but fails
    // to load still hard-fails at the TestNotNull below. Gated before sequence
    // creation so a skip leaves no probe asset behind.
    const TArray<FString> MannyMeshCandidates = {
        TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"),
        TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"),
        TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin"),
    };
    PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(MannyMeshCandidates);

    // Acceptance round-trip: add a CR track to a skeletal-mesh binding, key a control
    // at two frames, read it back. Only reachable once the handlers exist; pre-fix the
    // InvokeHandlerWithCapture calls return false (handler not found) and the success
    // assertions below fail alongside the registration checks.
    FString FullPath;
    ULevelSequence* Sequence = CreateControlRigTrackSequence(*this, FullPath);
    if (!Sequence)
    {
        // Registration failures above already mark this test Fail; without a sequence
        // factory there is nothing further to author.
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    // Pin a display rate distinct from the tick resolution BEFORE any track or key exists, so
    // the units assertions further down are discriminating rather than host-config dependent:
    // 24 fps display over 24000 ticks = 1000 ticks per displayed frame.
    MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
    MovieScene->SetDisplayRate(FFrameRate(24, 1));

    // Bind the possessable to a LIVE SK_Mannequin actor spawned in the editor world so
    // the FK Control Rig can generate its per-bone controls from a real reference
    // skeleton (an FK rig on an unbound possessable has zero controls — the red test's
    // own note above invites this fixture). The actor is destroyed on any exit.
    USkeletalMesh* MannyMesh = nullptr;
    for (const FString& Candidate : MannyMeshCandidates)
    {
        if (UEditorAssetLibrary::DoesAssetExist(Candidate))
        {
            MannyMesh = Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(Candidate));
            if (MannyMesh)
            {
                break;
            }
        }
    }
    if (!TestNotNull(TEXT("mannequin fixture skeletal mesh loaded"), MannyMesh))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world present"), EditorWorld))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    const FString MannyLabel = FString::Printf(TEXT("MCP_CRTrackManny_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ASkeletalMeshActor* MannyActor = SpawnActorInActiveWorld<ASkeletalMeshActor>(
        ASkeletalMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, MannyLabel);
    if (!TestNotNull(TEXT("SK_Mannequin actor spawned"), MannyActor))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    MannyActor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(MannyMesh);

    const FGuid BindingGuid = MovieScene->AddPossessable(MannyLabel, ASkeletalMeshActor::StaticClass());
    Sequence->BindPossessableObject(BindingGuid, *MannyActor, EditorWorld);
    const FString BindingId = BindingGuid.ToString(EGuidFormats::Digits);
    if (!TestTrue(TEXT("skeletal binding GUID is valid"), BindingGuid.IsValid()))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    // add_controlrig_track (FK fallback: no rigClass). Both param-name forms are set so
    // the eventual handler resolves the sequence + binding regardless of its aliases
    // (the direct-invoke path bypasses the dispatcher's UNKNOWN_PARAMS rejection, so
    // extra keys are harmless).
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), FullPath);
    AddPayload->SetStringField(TEXT("sequence"), FullPath);
    AddPayload->SetStringField(TEXT("bindingGuid"), BindingId);
    AddPayload->SetStringField(TEXT("binding"), BindingId);
    FTestResponseCapture AddCapture;
    const bool bAddFound = InvokeHandlerWithCapture(TEXT("sequencer.add_controlrig_track"), AddPayload, AddCapture);
    TestTrue(TEXT("sequencer.add_controlrig_track invoked (handler present)"), bAddFound);
    TestTrue(TEXT("add_controlrig_track reported success"), AddCapture.bSuccess);

    // list_controls -> at least one control; grab the first control name to key/read.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), FullPath);
    ListPayload->SetStringField(TEXT("sequence"), FullPath);
    ListPayload->SetStringField(TEXT("bindingGuid"), BindingId);
    ListPayload->SetStringField(TEXT("binding"), BindingId);
    FTestResponseCapture ListCapture;
    const bool bListFound = InvokeHandlerWithCapture(TEXT("sequencer.list_controls"), ListPayload, ListCapture);
    TestTrue(TEXT("sequencer.list_controls invoked (handler present)"), bListFound);
    TestTrue(TEXT("list_controls reported success"), ListCapture.bSuccess);

    FString ControlName;
    if (ListCapture.bSuccess && ListCapture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
        if (TestTrue(TEXT("list_controls returns a non-empty controls array"),
                ListCapture.Result->TryGetArrayField(TEXT("controls"), Controls) && Controls && Controls->Num() > 0))
        {
            // First entry may be a bare string name or an object carrying a "name" field.
            const TSharedPtr<FJsonValue>& First = (*Controls)[0];
            const TSharedPtr<FJsonObject>* AsObj = nullptr;
            if (First.IsValid() && First->TryGetObject(AsObj) && AsObj && (*AsObj).IsValid())
            {
                (*AsObj)->TryGetStringField(TEXT("name"), ControlName);
            }
            else if (First.IsValid())
            {
                First->TryGetString(ControlName);
            }
        }
    }

    if (ControlName.IsEmpty())
    {
        // Hard failure, not a quiet abandonment: the controls array was already asserted
        // non-empty just above, so an unreadable control name means list_controls changed the
        // shape of its entries (e.g. the `name` field written at
        // ControlRigSequencerHandler.cpp list_controls was renamed). That is a real
        // regression, not a host difference - and returning silently here drops key_controls,
        // get_control_value and the whole multi-channel round-trip from coverage while the
        // test still reports green.
        AddError(TEXT("PINWRIGHT-CR-CONTROL-NAME-UNREADABLE: sequencer.list_controls returned a "
                      "non-empty controls array but no readable control name (entry was neither "
                      "a bare string nor an object carrying a 'name' field). key_controls, "
                      "get_control_value and the multi-channel round-trip below are unreachable "
                      "and would otherwise be silently skipped."));
        CleanupTestAsset(FullPath);
        return true;
    }

    // key_controls: key the control at frame 0 (=0.0) and frame 30 (=1.0).
    auto KeyControlAtFrame = [&](double Frame, double Value) -> bool
    {
        TSharedPtr<FJsonObject> KeyPayload = MakeShared<FJsonObject>();
        KeyPayload->SetStringField(TEXT("path"), FullPath);
        KeyPayload->SetStringField(TEXT("sequence"), FullPath);
        KeyPayload->SetStringField(TEXT("bindingGuid"), BindingId);
        KeyPayload->SetStringField(TEXT("binding"), BindingId);
        KeyPayload->SetNumberField(TEXT("frame"), Frame);
        TSharedPtr<FJsonObject> ControlsMap = MakeShared<FJsonObject>();
        ControlsMap->SetNumberField(ControlName, Value);
        KeyPayload->SetObjectField(TEXT("controls"), ControlsMap);
        FTestResponseCapture KeyCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.key_controls"), KeyPayload, KeyCapture);
        return KeyCapture.bSuccess;
    };
    TestTrue(TEXT("key_controls succeeded at frame 0"), KeyControlAtFrame(0.0, 0.0));
    TestTrue(TEXT("key_controls succeeded at frame 30"), KeyControlAtFrame(30.0, 1.0));

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // UNITS (time axis). key_controls documents `frame` as a DISPLAY-rate frame number, and
    // the section's channels store TICK-resolution frame numbers. The value round-trip below
    // cannot see a units error, because key_controls and get_control_value key and read at the
    // SAME display frame through the SAME PinWrightControlRigSequencer::DisplayFrameToTick:
    // replace that helper's body with the identity and both land on tick 30, the readback still
    // returns 1.0, and every existing assertion stays green while the key sits at 0.00125 s
    // instead of 1.25 s. So read the STORED FFrameNumber straight off the channel and compare
    // it to the tick value re-derived from the MovieScene's own display rate and tick
    // resolution. ControlRig was the only keying verb in the sequencer surface with no such
    // units check.
    {
        const FFrameNumber ExpectedKeyTick = CRTrackExpectedTick(MovieScene, 30);

        // Precondition: display frames and ticks must differ here, or an identity conversion
        // would satisfy the assertion below and the check would prove nothing.
        if (TestTrue(TEXT("fixture discriminates: display frame 30 converts to a DIFFERENT tick "
                          "number (else display frames == ticks and the units check is degenerate)"),
                ExpectedKeyTick.Value != 30))
        {
            UMovieSceneControlRigParameterSection* KeyedSection =
                CRTrackFindKeyedSection(MovieScene, BindingGuid);
            if (TestNotNull(TEXT("Control Rig section resolvable for a direct channel read"),
                    KeyedSection))
            {
                // Scan every channel of the control, so this assertion is about the TIME axis
                // only and does not also depend on which channel the scalar write landed on.
                const TArray<FMovieSceneFloatChannel*> ControlChannels =
                    CRTrackControlChannelsByEngineIndex(KeyedSection, FName(*ControlName));
                bool bFoundExpectedTick = false;
                bool bFoundRawDisplayFrame = false;
                FString TimesDesc;
                for (const FMovieSceneFloatChannel* Channel : ControlChannels)
                {
                    const TArrayView<const FFrameNumber> Times = Channel->GetTimes();
                    for (int32 k = 0; k < Times.Num(); ++k)
                    {
                        bFoundExpectedTick |= (Times[k] == ExpectedKeyTick);
                        bFoundRawDisplayFrame |= (Times[k].Value == 30);
                        TimesDesc += FString::Printf(TEXT("%d "), Times[k].Value);
                    }
                }
                AddInfo(FString::Printf(
                    TEXT("control '%s' stored key times: [ %s] -- display frame 30 must store as "
                         "tick %d @ DisplayRate %d/%d, TickResolution %d/%d"),
                    *ControlName, *TimesDesc, ExpectedKeyTick.Value,
                    MovieScene->GetDisplayRate().Numerator, MovieScene->GetDisplayRate().Denominator,
                    MovieScene->GetTickResolution().Numerator, MovieScene->GetTickResolution().Denominator));

                if (TestTrue(TEXT("the control has at least one float channel to inspect"),
                        ControlChannels.Num() > 0))
                {
                    TestTrue(TEXT("key_controls stored display frame 30 as its TICK-resolution "
                                  "equivalent on the control's channels"),
                        bFoundExpectedTick);
                    TestFalse(TEXT("key_controls did NOT store the raw display frame number as a "
                                   "tick (the identity-conversion defect)"),
                        bFoundRawDisplayFrame);
                }
            }
        }
    }
#else
    AddWarning(TEXT("PINWRIGHT-CR-CHANNEL-METADATA-UNAVAILABLE: "
                    "UMovieSceneControlRigParameterSection::GetChannelMetaData and "
                    "UE::MovieScene::FControlRigChannelMetaData are 5.6+ only; skipping the "
                    "ControlRigTrack.AddKeyReadback display-frame-to-tick and channel-order "
                    "assertions on this engine version."));
#endif

    // get_control_value: read back at frame 30; acceptance = the keyed 1.0 within tolerance.
    TSharedPtr<FJsonObject> ReadPayload = MakeShared<FJsonObject>();
    ReadPayload->SetStringField(TEXT("path"), FullPath);
    ReadPayload->SetStringField(TEXT("sequence"), FullPath);
    ReadPayload->SetStringField(TEXT("bindingGuid"), BindingId);
    ReadPayload->SetStringField(TEXT("binding"), BindingId);
    ReadPayload->SetStringField(TEXT("control"), ControlName);
    ReadPayload->SetNumberField(TEXT("frame"), 30.0);
    FTestResponseCapture ReadCapture;
    const bool bReadFound = InvokeHandlerWithCapture(TEXT("sequencer.get_control_value"), ReadPayload, ReadCapture);
    TestTrue(TEXT("sequencer.get_control_value invoked (handler present)"), bReadFound);
    TestTrue(TEXT("get_control_value reported success"), ReadCapture.bSuccess);

    int32 ChannelCount = 0;
    if (ReadCapture.bSuccess && ReadCapture.Result.IsValid())
    {
        double EvaluatedValue = 0.0;
        if (TestTrue(TEXT("readback result carries a numeric value"),
                ReadCapture.Result->TryGetNumberField(TEXT("value"), EvaluatedValue)))
        {
            TestTrue(TEXT("control value at frame 30 is the keyed 1.0 within tolerance"),
                FMath::IsNearlyEqual(EvaluatedValue, 1.0, 0.01));
        }
        double CountVal = 0.0;
        if (ReadCapture.Result->TryGetNumberField(TEXT("channelCount"), CountVal))
        {
            ChannelCount = static_cast<int32>(CountVal);
        }
    }

    // Multi-channel round-trip (reviewer concern: key/read must touch every channel of a
    // transform/vector control, not only channel 0). Key the SAME control with a per-channel
    // array so all of its float channels are written, then read them all back via `values`.
    // For a single-channel control this still asserts the array-valued key path round-trips;
    // for a transform/vector control it covers the channels the scalar path leaves untouched.
    if (TestTrue(TEXT("get_control_value reports a positive channelCount"), ChannelCount > 0))
    {
        TArray<double> Targets;
        TArray<TSharedPtr<FJsonValue>> TargetsJson;
        for (int32 i = 0; i < ChannelCount; ++i)
        {
            const double T = 0.25 + 0.1 * static_cast<double>(i);
            Targets.Add(T);
            TargetsJson.Add(MakeShared<FJsonValueNumber>(T));
        }

        TSharedPtr<FJsonObject> MultiKeyPayload = MakeShared<FJsonObject>();
        MultiKeyPayload->SetStringField(TEXT("path"), FullPath);
        MultiKeyPayload->SetStringField(TEXT("sequence"), FullPath);
        MultiKeyPayload->SetStringField(TEXT("bindingGuid"), BindingId);
        MultiKeyPayload->SetStringField(TEXT("binding"), BindingId);
        MultiKeyPayload->SetNumberField(TEXT("frame"), 60.0);
        TSharedPtr<FJsonObject> MultiControls = MakeShared<FJsonObject>();
        MultiControls->SetArrayField(ControlName, TargetsJson);
        MultiKeyPayload->SetObjectField(TEXT("controls"), MultiControls);
        FTestResponseCapture MultiKeyCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.key_controls"), MultiKeyPayload, MultiKeyCapture);
        TestTrue(TEXT("key_controls accepts a per-channel array value"), MultiKeyCapture.bSuccess);

        TSharedPtr<FJsonObject> MultiReadPayload = MakeShared<FJsonObject>();
        MultiReadPayload->SetStringField(TEXT("path"), FullPath);
        MultiReadPayload->SetStringField(TEXT("sequence"), FullPath);
        MultiReadPayload->SetStringField(TEXT("bindingGuid"), BindingId);
        MultiReadPayload->SetStringField(TEXT("binding"), BindingId);
        MultiReadPayload->SetStringField(TEXT("control"), ControlName);
        MultiReadPayload->SetNumberField(TEXT("frame"), 60.0);
        FTestResponseCapture MultiReadCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.get_control_value"), MultiReadPayload, MultiReadCapture);
        TestTrue(TEXT("get_control_value succeeds for the multi-channel readback"), MultiReadCapture.bSuccess);

        if (MultiReadCapture.bSuccess && MultiReadCapture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* MultiValues = nullptr;
            if (TestTrue(TEXT("multi-channel readback carries a values array"),
                    MultiReadCapture.Result->TryGetArrayField(TEXT("values"), MultiValues) && MultiValues))
            {
                TestEqual(TEXT("values array length equals the control's channel count"),
                    MultiValues->Num(), ChannelCount);
                const int32 Compare = FMath::Min(MultiValues->Num(), Targets.Num());
                for (int32 i = 0; i < Compare; ++i)
                {
                    double Got = 0.0;
                    if ((*MultiValues)[i].IsValid())
                    {
                        (*MultiValues)[i]->TryGetNumber(Got);
                    }
                    TestTrue(*FString::Printf(TEXT("channel %d round-trips its keyed value"), i),
                        FMath::IsNearlyEqual(Got, Targets[i], 0.01));
                }
            }
        }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // CHANNEL ORDER. key_controls documents the array form as value[i] -> the control's
        // channel i in index order (transform = [TX,TY,TZ,RX,RY,RZ,SX,SY,SZ]). The `values`
        // readback above cannot see an ordering error: write and read both order channels
        // through the same PinWrightControlRigSequencer::ChannelsForControl sort, so inverting
        // that comparator scrambles the documented order while every value still round-trips
        // into its own slot. Anchor one end of the mapping to the ENGINE's own per-control
        // channel index instead, and the inversion becomes observable.
        {
            const FFrameNumber MultiKeyTick = CRTrackExpectedTick(MovieScene, 60);
            UMovieSceneControlRigParameterSection* KeyedSection =
                CRTrackFindKeyedSection(MovieScene, BindingGuid);
            if (TestNotNull(TEXT("Control Rig section resolvable for the channel-order read"),
                    KeyedSection))
            {
                const TArray<FMovieSceneFloatChannel*> EngineOrdered =
                    CRTrackControlChannelsByEngineIndex(KeyedSection, FName(*ControlName));
                if (TestEqual(TEXT("engine channel metadata reports the same channel count as "
                                   "get_control_value"),
                        EngineOrdered.Num(), ChannelCount))
                {
                    for (int32 i = 0; i < EngineOrdered.Num() && i < Targets.Num(); ++i)
                    {
                        float Stored = 0.0f;
                        EngineOrdered[i]->Evaluate(FFrameTime(MultiKeyTick), Stored);
                        TestTrue(*FString::Printf(
                                     TEXT("the channel the ENGINE indexes as %d holds the value "
                                          "keyed for slot %d (expected %.4f, stored %.4f) -- the "
                                          "documented [TX,TY,TZ,RX,RY,RZ,SX,SY,SZ] order"),
                                     i, i, Targets[i], static_cast<double>(Stored)),
                            FMath::IsNearlyEqual(static_cast<double>(Stored), Targets[i], 0.01));
                    }
                }
            }
        }
#endif
    }

    CleanupTestAsset(FullPath);
    return true;
}
