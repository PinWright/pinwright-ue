// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-sequencer-transform-track-unkeyed-defaults.
//
// UMovieScene3DTransformSection's constructor gives all nine channels a DEFAULT (location 0,
// rotation 0, scale 1), and the engine evaluates a channel that holds keys OR a default. Transform
// sections the sequencer verbs created kept those defaults, so a binding keyed on rotation only
// snapped its object to the world origin at scale 1 on playback, and no verb could author a
// partial track or unpin an existing one.
//
// The decisive assertions play the sequence onto a real level actor placed off the origin, at a
// non-unit scale and a non-zero roll, and read the actor back: pre-fix the location and scale snap
// to 0 / 1 and the roll to 0. The readback fields (transformMask, channelDefaults,
// evaluatedChannels, unanimatedChannels) are asserted beside that, never instead of it.
//
// No conditional-skip path: the only fixture content is /Engine/BasicShapes/Cube.Cube, and a
// missing editor world or sequence factory is a hard AddError.

#include "Misc/AutomationTest.h"
#include "Tests/AutomationSuiteMaintenance.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"

#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/AssetUtils.h"

namespace
{
    // Where the probe actor stands before any sequence touches it: off the origin, scaled and
    // rolled, so a channel pinned to a constructor default (0 / 1) is visible on every group.
    const FVector PWXfChanStartLocation(1234.0, -567.0, 89.0);
    const FVector PWXfChanStartScale(2.0, 2.0, 2.0);
    constexpr double PWXfChanStartRoll = 15.0;

    // Yaw ramps 0 -> 90 linearly over display frames 0 -> 30, so frame 15 is 45 and not a key.
    constexpr int32 PWXfChanEndFrame = 30;
    constexpr int32 PWXfChanEvalFrame = 15;
    constexpr double PWXfChanEndYaw = 90.0;
    constexpr double PWXfChanEvalYaw = 45.0;

    ULevelSequence* CreatePWXfChanSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_XfChanSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = FString(PinWrightSuiteMaintenance::ScratchRootPackagePath()) / TEXT("MCP_XfChanProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);
        if (!CreateCapture.bSuccess || !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create."));
            OutFullPath.Reset();
            return nullptr;
        }
        ULevelSequence* Sequence = Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
        if (Sequence && Sequence->GetMovieScene())
        {
            UMovieScene* MovieScene = Sequence->GetMovieScene();
            const FFrameNumber EndTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(PWXfChanEndFrame)),
                MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).FloorToFrame();
            MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndTick + FFrameNumber(1)));
        }
        return Sequence;
    }

    // A normal (non-transient) movable level actor at the start pose, possessed by a new binding.
    AStaticMeshActor* SpawnAndBindPWXfChanActor(UWorld* World, ULevelSequence* Sequence, FGuid& OutBinding)
    {
        OutBinding.Invalidate();
        const FString Label = FString::Printf(TEXT("PWXfChanProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        AStaticMeshActor* Actor = SpawnActorInActiveWorld<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
            PWXfChanStartLocation, FRotator(0.0, 0.0, PWXfChanStartRoll), Label);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(
            LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        // AStaticMeshActor is Static by default; a Static component ignores Sequencer's transform.
        Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        Actor->SetActorScale3D(PWXfChanStartScale);

        UMovieScene* MovieScene = Sequence->GetMovieScene();
        OutBinding = MovieScene->AddPossessable(Label, Actor->GetClass());
        Sequence->BindPossessableObject(OutBinding, *Actor, World);
        return Actor;
    }

    // Evaluate the sequence onto the world at one display frame and read the actor back. The
    // player is stopped (restoring the actor) and its actor destroyed before returning.
    bool PlayPWXfChanFrame(FAutomationTestBase& Test, UWorld* World, ULevelSequence* Sequence,
        AActor* Actor, int32 DisplayFrame, FTransform& OutTransform)
    {
        ALevelSequenceActor* PlayerActor = nullptr;
        ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(
            World, Sequence, FMovieSceneSequencePlaybackSettings(), PlayerActor);
        if (!Player)
        {
            Test.AddError(TEXT("Could not create a ULevelSequencePlayer for the editor world."));
            return false;
        }
        // ALevelSequenceActor::InitializePlayer no-ops outside a game world.
        Player->Initialize(Sequence, World->PersistentLevel, FLevelSequenceCameraSettings());
        Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
            FFrameTime(FFrameNumber(DisplayFrame)), EUpdatePositionMethod::Jump));
        OutTransform = Actor->GetActorTransform();
        Player->Stop();
        if (IsValid(PlayerActor))
        {
            PlayerActor->Destroy();
        }
        return true;
    }

    TArray<FString> PWXfChanStrings(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Obj.IsValid() && Obj->TryGetArrayField(Field, Values) && Values)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FString Text;
                if (Value.IsValid() && Value->TryGetString(Text))
                {
                    Out.Add(Text);
                }
            }
        }
        return Out;
    }

    int32 PWXfChanDefaultCount(const TSharedPtr<FJsonObject>& Obj)
    {
        const TSharedPtr<FJsonObject>* Defaults = nullptr;
        return (Obj.IsValid() && Obj->TryGetObjectField(TEXT("channelDefaults"), Defaults) && Defaults)
            ? (*Defaults)->Values.Num() : -1;
    }

    UMovieScene3DTransformSection* PWXfChanFirstSection(UMovieScene* MovieScene, const FGuid& Binding)
    {
        UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(Binding);
        if (!Track)
        {
            return nullptr;
        }
        for (UMovieSceneSection* Section : Track->GetAllSections())
        {
            if (UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(Section))
            {
                return TransformSection;
            }
        }
        return nullptr;
    }

    bool PWXfChanEnvironment(FAutomationTestBase& Test, UWorld*& OutWorld)
    {
        OutWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!OutWorld || !OutWorld->PersistentLevel)
        {
            Test.AddError(TEXT("No editor world with a persistent level to play the probe sequence into."));
            return false;
        }
        return true;
    }

    // Location, scale and roll are the actor's own; only yaw follows the keys.
    void ExpectPWXfChanStartPoseWithYaw(FAutomationTestBase& Test, const FString& Phase,
        const FTransform& Played, double ExpectedYaw)
    {
        const FRotator Rotation = Played.Rotator();
        Test.TestTrue(FString::Printf(TEXT("%s: location stays the actor's own %s (got %s; a pinned "
                "default reads 0,0,0)"), *Phase, *PWXfChanStartLocation.ToString(), *Played.GetLocation().ToString()),
            Played.GetLocation().Equals(PWXfChanStartLocation, 0.5));
        Test.TestTrue(FString::Printf(TEXT("%s: scale stays the actor's own %s (got %s; a pinned "
                "default reads 1,1,1)"), *Phase, *PWXfChanStartScale.ToString(), *Played.GetScale3D().ToString()),
            Played.GetScale3D().Equals(PWXfChanStartScale, 0.01));
        Test.TestTrue(FString::Printf(TEXT("%s: unkeyed roll stays %g (got %g; a pinned default reads 0)"),
                *Phase, PWXfChanStartRoll, Rotation.Roll),
            FMath::IsNearlyEqual(Rotation.Roll, PWXfChanStartRoll, 0.5));
        Test.TestTrue(FString::Printf(TEXT("%s: keyed yaw is %g (got %g)"), *Phase, ExpectedYaw, Rotation.Yaw),
            FMath::IsNearlyEqual(Rotation.Yaw, ExpectedYaw, 0.5));
    }

    // Key yaw (Rotation.Z) directly on a section, linear 0 -> PWXfChanEndYaw.
    void KeyPWXfChanYawRamp(UMovieScene* MovieScene, UMovieScene3DTransformSection* Section)
    {
        const FFrameNumber EndTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(PWXfChanEndFrame)),
            MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).FloorToFrame();
        FMovieSceneDoubleChannel* Yaw = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>()[5];
        FMovieSceneDoubleValue Start(0.0);
        Start.InterpMode = RCIM_Linear;
        FMovieSceneDoubleValue End(PWXfChanEndYaw);
        End.InterpMode = RCIM_Linear;
        Yaw->GetData().UpdateOrAddKey(FFrameNumber(0), Start);
        Yaw->GetData().UpdateOrAddKey(EndTick, End);
        Section->SetRange(TRange<FFrameNumber>::All());
    }
}

// ============================================================================
// Rotation-only keys through sequencer.add_keyframes leave everything else to the actor
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWXfChanRotationOnlyKeysTest,
    "PinWright.Sequencer.TransformChannels.RotationOnlyKeysLeaveLocationAndScaleToTheActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWXfChanRotationOnlyKeysTest::RunTest(const FString& Parameters)
{
    UWorld* World = nullptr;
    if (!PWXfChanEnvironment(*this, World))
    {
        return false;
    }
    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreatePWXfChanSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();

    FGuid Binding;
    AStaticMeshActor* Actor = SpawnAndBindPWXfChanActor(World, Sequence, Binding);
    if (!TestNotNull(TEXT("probe actor spawned and bound"), Actor) || !TestTrue(TEXT("binding valid"), Binding.IsValid()))
    {
        return false;
    }

    // Yaw only: the batch verb creates the track and its section.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), SeqPath);
    Payload->SetStringField(TEXT("bindingId"), Binding.ToString());
    Payload->SetStringField(TEXT("property"), TEXT("Rotation"));
    Payload->SetStringField(TEXT("interp"), TEXT("linear"));
    TArray<TSharedPtr<FJsonValue>> Keys;
    for (const TPair<int32, double>& FrameYaw : { TPair<int32, double>(0, 0.0), TPair<int32, double>(PWXfChanEndFrame, PWXfChanEndYaw) })
    {
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetNumberField(TEXT("yaw"), FrameYaw.Value);
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("frame"), FrameYaw.Key);
        Key->SetObjectField(TEXT("value"), Value);
        Keys.Add(MakeShared<FJsonValueObject>(Key));
    }
    Payload->SetArrayField(TEXT("keys"), Keys);
    FTestResponseCapture KeyCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.add_keyframes"), Payload, KeyCapture);
    if (!TestTrue(FString::Printf(TEXT("add_keyframes succeeded (code=%s msg=%s)"), *KeyCapture.ErrorCode,
            *KeyCapture.Message), KeyCapture.bSuccess))
    {
        return false;
    }

    // Authored state: only Rotation.Z holds anything.
    UMovieScene3DTransformSection* Section = PWXfChanFirstSection(MovieScene, Binding);
    if (!TestNotNull(TEXT("add_keyframes created a transform section"), Section))
    {
        return false;
    }
    TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    for (int32 Index = 0; Index < 9 && Index < Channels.Num(); ++Index)
    {
        TestFalse(FString::Printf(TEXT("channel %d carries no default on a section the verb created"), Index),
            Channels[Index]->GetDefault().IsSet());
    }

    // Played state: the decisive measurement.
    FTransform Played;
    if (PlayPWXfChanFrame(*this, World, Sequence, Actor, PWXfChanEvalFrame, Played))
    {
        ExpectPWXfChanStartPoseWithYaw(*this, TEXT("rotation-only track"), Played, PWXfChanEvalYaw);
    }

    // Readbacks agree with what played.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), SeqPath);
    ListPayload->SetStringField(TEXT("bindingGuid"), Binding.ToString());
    FTestResponseCapture ListCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), ListPayload, ListCapture);
    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (TestTrue(TEXT("list_sections returned sections"), ListCapture.bSuccess &&
            ListCapture.Result->TryGetArrayField(TEXT("sections"), Rows) && Rows && Rows->Num() == 1))
    {
        const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
        TestEqual(TEXT("list_sections evaluatedChannels is exactly Rotation.Z"),
            FString::Join(PWXfChanStrings(Row, TEXT("evaluatedChannels")), TEXT(",")), FString(TEXT("Rotation.Z")));
        TestEqual(TEXT("list_sections transformMask is the full nine-channel default"),
            PWXfChanStrings(Row, TEXT("transformMask")).Num(), 9);
        TestEqual(TEXT("list_sections channelDefaults is empty"), PWXfChanDefaultCount(Row), 0);
    }

    TSharedPtr<FJsonObject> EvalPayload = MakeShared<FJsonObject>();
    EvalPayload->SetStringField(TEXT("path"), SeqPath);
    EvalPayload->SetStringField(TEXT("bindingId"), Binding.ToString());
    EvalPayload->SetNumberField(TEXT("frame"), PWXfChanEvalFrame);
    FTestResponseCapture EvalCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.get_binding_transform"), EvalPayload, EvalCapture);
    if (TestTrue(FString::Printf(TEXT("get_binding_transform succeeded on a partial track (code=%s msg=%s)"),
            *EvalCapture.ErrorCode, *EvalCapture.Message), EvalCapture.bSuccess))
    {
        const TArray<FString> Unanimated = PWXfChanStrings(EvalCapture.Result, TEXT("unanimatedChannels"));
        TestEqual(TEXT("get_binding_transform names the eight channels the track leaves alone"), Unanimated.Num(), 8);
        TestFalse(TEXT("unanimatedChannels does not list the keyed Rotation.Z"), Unanimated.Contains(TEXT("Rotation.Z")));
        const TSharedPtr<FJsonObject>* Rot = nullptr;
        double Yaw = 0.0;
        TestTrue(TEXT("get_binding_transform reports the interpolated yaw"),
            EvalCapture.Result->TryGetObjectField(TEXT("rotation"), Rot) && Rot &&
            (*Rot)->TryGetNumberField(TEXT("yaw"), Yaw) && FMath::IsNearlyEqual(Yaw, PWXfChanEvalYaw, 0.5));
    }
    return true;
}

// ============================================================================
// Every creating verb leaves the new section without defaults
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWXfChanCreatedSectionsHaveNoDefaultsTest,
    "PinWright.Sequencer.TransformChannels.CreatedSectionsCarryNoChannelDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWXfChanCreatedSectionsHaveNoDefaultsTest::RunTest(const FString& Parameters)
{
    FString SeqPath;
    ULevelSequence* Sequence = CreatePWXfChanSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    const FString PossessableName = TEXT("PWXfChanTrackProbe");
    const FGuid Binding = MovieScene->AddPossessable(PossessableName, AStaticMeshActor::StaticClass());

    TSharedPtr<FJsonObject> TrackPayload = MakeShared<FJsonObject>();
    TrackPayload->SetStringField(TEXT("sequencePath"), SeqPath);
    TrackPayload->SetStringField(TEXT("bindingGuid"), Binding.ToString());
    FTestResponseCapture TrackCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.add_transform_track"), TrackPayload, TrackCapture);
    if (!TestTrue(TEXT("add_transform_track succeeded"), TrackCapture.bSuccess))
    {
        return false;
    }
    TestEqual(TEXT("add_transform_track response: channelDefaults is empty"), PWXfChanDefaultCount(TrackCapture.Result), 0);
    TestEqual(TEXT("add_transform_track response: evaluatedChannels is empty"),
        PWXfChanStrings(TrackCapture.Result, TEXT("evaluatedChannels")).Num(), 0);
    TestEqual(TEXT("add_transform_track response: transformMask lists all nine channels"),
        PWXfChanStrings(TrackCapture.Result, TEXT("transformMask")).Num(), 9);

    // A second section on the same track through the generic verb.
    TSharedPtr<FJsonObject> SectionPayload = MakeShared<FJsonObject>();
    SectionPayload->SetStringField(TEXT("path"), SeqPath);
    SectionPayload->SetStringField(TEXT("trackName"), TEXT("MovieScene3DTransformTrack"));
    SectionPayload->SetStringField(TEXT("actorName"), PossessableName);
    SectionPayload->SetNumberField(TEXT("startFrame"), 0);
    SectionPayload->SetNumberField(TEXT("endFrame"), 10);
    FTestResponseCapture SectionCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.add_section"), SectionPayload, SectionCapture);
    TestTrue(FString::Printf(TEXT("add_section on the Transform track succeeded (code=%s msg=%s)"),
        *SectionCapture.ErrorCode, *SectionCapture.Message), SectionCapture.bSuccess);

    UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(Binding);
    if (!TestNotNull(TEXT("transform track exists"), Track))
    {
        return false;
    }
    TestEqual(TEXT("track holds the add_transform_track and add_section sections"), Track->GetAllSections().Num(), 2);
    for (int32 SectionIndex = 0; SectionIndex < Track->GetAllSections().Num(); ++SectionIndex)
    {
        UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[SectionIndex]);
        if (!TestNotNull(TEXT("section is a 3D transform section"), Section))
        {
            continue;
        }
        for (FMovieSceneDoubleChannel* Channel : Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>())
        {
            TestFalse(FString::Printf(TEXT("section %d: no channel keeps a constructor default"), SectionIndex),
                Channel->GetDefault().IsSet());
        }
    }
    return true;
}

// ============================================================================
// set_transform_mask unpins a section that still carries constructor defaults
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWXfChanMaskUnpinsLegacySectionTest,
    "PinWright.Sequencer.TransformChannels.SetTransformMaskUnpinsASectionWithDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWXfChanMaskUnpinsLegacySectionTest::RunTest(const FString& Parameters)
{
    UWorld* World = nullptr;
    if (!PWXfChanEnvironment(*this, World))
    {
        return false;
    }
    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreatePWXfChanSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();

    FGuid Binding;
    AStaticMeshActor* Actor = SpawnAndBindPWXfChanActor(World, Sequence, Binding);
    if (!TestNotNull(TEXT("probe actor spawned and bound"), Actor))
    {
        return false;
    }

    // The pre-fix shape, built directly: a section straight from CreateNewSection keeps the
    // constructor defaults on all nine channels, with only yaw keyed.
    UMovieScene3DTransformTrack* Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(Binding);
    UMovieScene3DTransformSection* Section = Track ? Cast<UMovieScene3DTransformSection>(Track->CreateNewSection()) : nullptr;
    if (!TestNotNull(TEXT("legacy transform section created"), Section))
    {
        return false;
    }
    Track->AddSection(*Section);
    KeyPWXfChanYawRamp(MovieScene, Section);

    // Fixture check: this section must reproduce the defect, or the repair below proves nothing.
    FTransform Pinned;
    if (PlayPWXfChanFrame(*this, World, Sequence, Actor, PWXfChanEvalFrame, Pinned))
    {
        TestTrue(FString::Printf(TEXT("fixture: the defaulted section pins the actor to the origin (got %s)"),
                *Pinned.GetLocation().ToString()), Pinned.GetLocation().IsNearlyZero(0.5));
    }
    // Put the start pose back explicitly rather than relying on the player's state restore.
    Actor->SetActorTransform(FTransform(FRotator(0.0, 0.0, PWXfChanStartRoll), PWXfChanStartLocation, PWXfChanStartScale));

    TSharedPtr<FJsonObject> MaskPayload = MakeShared<FJsonObject>();
    MaskPayload->SetStringField(TEXT("path"), SeqPath);
    MaskPayload->SetStringField(TEXT("bindingId"), Binding.ToString());
    MaskPayload->SetArrayField(TEXT("channels"),
        TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(TEXT("rotation.z")) });
    FTestResponseCapture MaskCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.set_transform_mask"), MaskPayload, MaskCapture);
    if (!TestTrue(FString::Printf(TEXT("set_transform_mask succeeded (code=%s msg=%s)"), *MaskCapture.ErrorCode,
            *MaskCapture.Message), MaskCapture.bSuccess))
    {
        return false;
    }

    TestEqual(TEXT("section mask is now Rotation.Z only"),
        static_cast<int64>(Section->GetMask().GetChannels()), static_cast<int64>(EMovieSceneTransformChannel::RotationZ));
    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (TestTrue(TEXT("response carries one section row"),
            MaskCapture.Result->TryGetArrayField(TEXT("sections"), Rows) && Rows && Rows->Num() == 1))
    {
        const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
        TestEqual(TEXT("previousMask lists the nine channels the section had"), PWXfChanStrings(Row, TEXT("previousMask")).Num(), 9);
        TestEqual(TEXT("transformMask reads back Rotation.Z"), FString::Join(PWXfChanStrings(Row, TEXT("transformMask")), TEXT(",")), FString(TEXT("Rotation.Z")));
        TestEqual(TEXT("evaluatedChannels reads back Rotation.Z"), FString::Join(PWXfChanStrings(Row, TEXT("evaluatedChannels")), TEXT(",")), FString(TEXT("Rotation.Z")));
        TestEqual(TEXT("the defaults are still there, only masked out"), PWXfChanDefaultCount(Row), 9);
    }

    FTransform Repaired;
    if (PlayPWXfChanFrame(*this, World, Sequence, Actor, PWXfChanEvalFrame, Repaired))
    {
        ExpectPWXfChanStartPoseWithYaw(*this, TEXT("masked legacy section"), Repaired, PWXfChanEvalYaw);
    }
    return true;
}

// ============================================================================
// set_transform_mask refuses what it cannot apply, and writes nothing
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPWXfChanMaskRefusalsTest,
    "PinWright.Sequencer.TransformChannels.SetTransformMaskRefusesWithoutWriting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPWXfChanMaskRefusalsTest::RunTest(const FString& Parameters)
{
    FString SeqPath;
    ULevelSequence* Sequence = CreatePWXfChanSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    const FGuid Bare = MovieScene->AddPossessable(TEXT("PWXfChanBare"), AStaticMeshActor::StaticClass());
    const FGuid Tracked = MovieScene->AddPossessable(TEXT("PWXfChanTracked"), AStaticMeshActor::StaticClass());
    UMovieScene3DTransformTrack* Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(Tracked);
    UMovieScene3DTransformSection* Section = Track ? Cast<UMovieScene3DTransformSection>(Track->CreateNewSection()) : nullptr;
    if (!TestNotNull(TEXT("transform section created"), Section))
    {
        return false;
    }
    Track->AddSection(*Section);
    const int64 MaskBefore = static_cast<int64>(Section->GetMask().GetChannels());

    auto Call = [&](const FGuid& Binding, const TArray<TSharedPtr<FJsonValue>>& Channels, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), SeqPath);
        Payload->SetStringField(TEXT("bindingId"), Binding.ToString());
        Payload->SetArrayField(TEXT("channels"), Channels);
        InvokeHandlerWithCapture(TEXT("sequencer.set_transform_mask"), Payload, Capture);
    };

    struct FCase { const TCHAR* What; TArray<TSharedPtr<FJsonValue>> Channels; };
    const FCase Cases[] = {
        { TEXT("an unknown channel name next to a valid one"),
            { MakeShared<FJsonValueString>(TEXT("Rotation")), MakeShared<FJsonValueString>(TEXT("Location.W")) } },
        { TEXT("an empty channel list"), {} },
        { TEXT("a non-string entry"), { MakeShared<FJsonValueNumber>(3.0) } },
    };
    for (const FCase& Case : Cases)
    {
        FTestResponseCapture Capture;
        Call(Tracked, Case.Channels, Capture);
        TestFalse(FString::Printf(TEXT("%s is refused"), Case.What), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s is INVALID_ARGUMENT"), Case.What), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestEqual(FString::Printf(TEXT("%s leaves the mask unchanged"), Case.What),
            static_cast<int64>(Section->GetMask().GetChannels()), MaskBefore);
    }

    FTestResponseCapture NoTrack;
    Call(Bare, { MakeShared<FJsonValueString>(TEXT("Rotation")) }, NoTrack);
    TestEqual(TEXT("a binding without a Transform track is NO_TRANSFORM_TRACK"), NoTrack.ErrorCode, FString(TEXT("NO_TRANSFORM_TRACK")));
    TestNull(TEXT("the refusal does not create a Transform track"), MovieScene->FindTrack<UMovieScene3DTransformTrack>(Bare));
    return true;
}
