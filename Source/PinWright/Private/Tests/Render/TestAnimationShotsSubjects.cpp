// Copyright (c) 2026 Alexander Penkin. MIT License.

// The RESPONSE-SHAPE floor for camera.animation_shots, plus its subject-descriptor surface.
//
// WHY THIS FILE EXISTS AT ALL. Before it, the verb had no reachable shape coverage. Its only
// response-shape assertions live in TestAnimationCaptureHandlers.cpp behind
// `if (!Capture.bSuccess) { return true; }`, on a fixture that is a StaticMeshActor the verb
// always refuses with ACTOR_NO_SKELETAL_MESH_COMPONENT — the file says so itself at the block's
// head. Every one of those assertions is dead code on every run, and the verb was about to be
// converted onto PinWrightPoseCapture::CaptureCameraPoses with nothing standing between the
// refactor and a silently different response. That block stays where it is and is not repaired
// here: it is owned by nobody and must pass unmodified. This file is the reachable version of it.
//
// WHAT IT PINS, and what each group would fail to catch on its own:
//
//  * The argument surface (deterministic, no world, no RHI). `subject`, `viewMode`,
//    `distribution` and `seed` must be DECLARED, not merely readable from the payload: an
//    undeclared parameter is rejected by the dispatcher's unknown-args gate before the handler
//    ever runs, so a body that reads one is a parameter that cannot be passed.
//  * The typed refusal for a subject kind with no time axis (deterministic). A static mesh has
//    no time to scrub to, and the refusal has to be UNSUPPORTED_ASSET_EDITOR naming the missing
//    axis rather than the dispatcher's UNKNOWN_PARAMS, which tells a caller nothing about why.
//    Asserted in BOTH directions — the code is right AND it is not UNKNOWN_PARAMS — because the
//    whole point of the change is which of those two a caller sees.
//  * The burst response shape, against a fixture that actually animates (guarded). Every
//    top-level and per-shot key is listed as a LITERAL in the test, so a key silently dropped by
//    a refactor names itself in the failure instead of vanishing.
//  * That ONE camera and ONE capture size serve the whole burst. Both are properties the verb
//    exists for: a camera re-solved per instant moves under the subject and destroys the
//    comparison, and a capture size that varies within a session trips
//    `Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in FViewport::GetHitProxy — an
//    incident that cost 66 actors and 125 emitters of unsaved level state.
//
// THE FIXTURE MOVES, AND THAT IS ASSERTED FIRST. A one-camera assertion against a fixture that
// never moves cannot fail: the bounds union would equal the single-instant bounds and the camera
// would be identical for a reason that has nothing to do with the rule under test. So the fixture
// carries a real UMovieScene3DTransformTrack with two Z keys, and the test asserts a non-zero
// `poseDeltaFromFirst.componentTranslationCm` as a PRECONDITION before it asserts the camera held.
//
// EVERY SKIP ANNOUNCES ITSELF. The burst needs a live Level Editor viewport and an RHI, neither
// of which a headless host has. Those guards are correct, but a silent skip reports a verdict
// about a property it never examined — board ticket `B-test-skips-assertions-silently`. Each
// early return emits the `PINWRIGHT_ASSERTIONS_SKIPPED:` marker that
// `Content/Python/check_suite_log.py` reconciles into its own `skipped` outcome, as a WARNING and
// never a failure.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"

#include "Utils/AssetUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Typed, non-crashing exits the burst is allowed to return when it cannot complete on this
    // host. Deliberately the same list as PWAnimShotsIsTypedFailure in
    // TestAnimationCaptureHandlers.cpp, minus ACTOR_NO_SKELETAL_MESH_COMPONENT: this file's
    // fixture IS a skeletal mesh actor, so that code would be a real defect here rather than an
    // environment difference. An empty or unlisted code still fails.
    bool PWAnimSubjIsEnvironmentalFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("SEQUENCE_NOT_OPEN") ||
            ErrorCode == TEXT("EXECUTION_ERROR") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED");
    }

    double PWAnimSubjNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double Fallback)
    {
        double Value = Fallback;
        if (Obj.IsValid())
        {
            Obj->TryGetNumberField(Key, Value);
        }
        return Value;
    }

    const TSharedPtr<FJsonObject>* PWAnimSubjObject(const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* Key)
    {
        const TSharedPtr<FJsonObject>* Out = nullptr;
        if (Obj.IsValid() && Obj->TryGetObjectField(Key, Out))
        {
            return Out;
        }
        return nullptr;
    }

    // ---- the moving fixture ----
    //
    // A real skeletal-mesh actor in the editor world, a real /Game LevelSequence possessing it,
    // and a real transform track that MOVES it 1200 cm in Z between display frames 0 and 10.
    // Everything is torn down in the destructor, including the persistent level's dirty flag —
    // a non-transient spawn dirties the open map, and automation runs against whatever map the
    // editor started on.
    //
    // The mesh is /Engine/EngineMeshes/SkeletalCube rather than a mannequin: it ships with the
    // engine, so this fixture cannot become a host-dependent skip on a project that does not
    // carry Lyra content.
    class FPWAnimSubjMovingFixture
    {
    public:
        static constexpr int32 FirstDisplayFrame = 0;
        static constexpr int32 LastDisplayFrame = 10;
        static constexpr double FirstZ = 300.0;
        static constexpr double LastZ = 1500.0;

        FPWAnimSubjMovingFixture()
        {
            UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            if (!World)
            {
                FailureReason = TEXT("no editor world");
                return;
            }

            static const TCHAR* MeshCandidates[] = {
                TEXT("/Engine/EngineMeshes/SkeletalCube"),
                TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"),
                TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"),
            };
            USkeletalMesh* Mesh = nullptr;
            for (const TCHAR* Candidate : MeshCandidates)
            {
                if (UEditorAssetLibrary::DoesAssetExist(Candidate))
                {
                    Mesh = Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(Candidate));
                    if (Mesh)
                    {
                        break;
                    }
                }
            }
            if (!Mesh)
            {
                FailureReason = TEXT("no loadable skeletal mesh (engine SkeletalCube unavailable)");
                return;
            }

            ActorLabel = FString::Printf(TEXT("MCP_AnimShotsSubjActor_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            ASkeletalMeshActor* Actor = SpawnActorInActiveWorld<ASkeletalMeshActor>(
                ASkeletalMeshActor::StaticClass(),
                FVector(0.0, 0.0, FirstZ), FRotator::ZeroRotator, ActorLabel);
            if (!Actor)
            {
                FailureReason = TEXT("could not spawn a SkeletalMeshActor in the editor world");
                return;
            }
            Actor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);
            // Read the label back rather than assuming SetActorLabel stored it verbatim — the
            // handler resolves through McpActorUtils::FindActorByName and must see this string.
            ActorLabel = Actor->GetActorLabel();

            if (!CreateSequence())
            {
                return;
            }
            if (!BindAndKeyTheActor())
            {
                return;
            }
            bReady = true;
        }

        ~FPWAnimSubjMovingFixture()
        {
            // ActorGuard destructs after this body, removing the spawned actor and restoring the
            // level's dirty flag. The sequence asset is ours to delete.
            if (!SequencePath.IsEmpty())
            {
                CleanupTestAsset(SequencePath);
            }
        }

        bool IsReady() const { return bReady; }
        const FString& GetFailureReason() const { return FailureReason; }
        const FString& GetSequencePath() const { return SequencePath; }
        const FString& GetActorLabel() const { return ActorLabel; }

    private:
        bool CreateSequence()
        {
            const FString SeqName = FString::Printf(TEXT("MCP_AnimShotsSubjSeq_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            const FString DestFolder = TEXT("/Game/MCP_AnimShotsSubjProbe");

            TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
            CreatePayload->SetStringField(TEXT("name"), SeqName);
            CreatePayload->SetStringField(TEXT("path"), DestFolder);
            FTestResponseCapture CreateCapture;
            InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

            const FString FullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);
            if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
                !UEditorAssetLibrary::DoesAssetExist(FullPath))
            {
                FailureReason = TEXT("sequencer.create could not make a probe LevelSequence");
                return false;
            }
            SequencePath = FullPath;
            Sequence = Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(SequencePath));
            if (!Sequence || !Sequence->GetMovieScene())
            {
                FailureReason = TEXT("the probe LevelSequence has no MovieScene");
                return false;
            }
            return true;
        }

        // Possess the actor through the production sequencer.add_actor handler (so the binding is
        // made the way a caller would make it), then author the transform keys directly on the
        // MovieScene. The keys are written in C++ rather than through sequence.add_keyframe
        // because the fixture's value is that the actor PROVABLY moves, and going through a
        // second verb would make a defect in that verb read as a defect in this one.
        bool BindAndKeyTheActor()
        {
            TSharedPtr<FJsonObject> BindPayload = MakeShared<FJsonObject>();
            BindPayload->SetStringField(TEXT("path"), SequencePath);
            BindPayload->SetStringField(TEXT("actorName"), ActorLabel);
            FTestResponseCapture BindCapture;
            InvokeHandlerWithCapture(TEXT("sequencer.add_actor"), BindPayload, BindCapture);

            FString BindingGuidString;
            const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
            if (BindCapture.bSuccess && BindCapture.Result.IsValid() &&
                BindCapture.Result->TryGetArrayField(TEXT("results"), Results) &&
                Results && Results->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* First = nullptr;
                if ((*Results)[0].IsValid() && (*Results)[0]->TryGetObject(First) && First)
                {
                    (*First)->TryGetStringField(TEXT("bindingGuid"), BindingGuidString);
                }
            }
            FGuid BindingGuid;
            if (BindingGuidString.IsEmpty() || !FGuid::Parse(BindingGuidString, BindingGuid))
            {
                FailureReason = TEXT("sequencer.add_actor produced no bindingGuid");
                return false;
            }

            UMovieScene* MovieScene = Sequence->GetMovieScene();
            const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
            const FFrameRate TickResolution = MovieScene->GetTickResolution();
            const auto ToTick = [DisplayRate, TickResolution](int32 DisplayFrame)
            {
                return FFrameRate::TransformTime(FFrameTime(FFrameNumber(DisplayFrame)),
                    DisplayRate, TickResolution).GetFrame();
            };

            UMovieScene3DTransformTrack* Track =
                MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
            if (!Track)
            {
                Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
            }
            if (!Track)
            {
                FailureReason = TEXT("could not add a UMovieScene3DTransformTrack to the binding");
                return false;
            }

            bool bSectionAdded = false;
            UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(
                Track->FindOrAddSection(ToTick(FirstDisplayFrame), bSectionAdded));
            if (!Section)
            {
                FailureReason = TEXT("could not resolve a UMovieScene3DTransformSection");
                return false;
            }

            // Channel layout on a transform section: 0-2 Location, 3-5 Rotation, 6-8 Scale.
            // Only Z is keyed, so X and Y keep whatever the actor was spawned with — a moving
            // subject in one axis is all the precondition needs, and fewer keys is fewer ways for
            // the fixture to be the thing that broke.
            TArrayView<FMovieSceneDoubleChannel*> Channels =
                Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
            if (Channels.Num() < 3 || !Channels[2])
            {
                FailureReason = TEXT("the transform section exposes no Location.Z double channel");
                return false;
            }
            Channels[2]->GetData().AddKey(ToTick(FirstDisplayFrame), FMovieSceneDoubleValue(FirstZ));
            Channels[2]->GetData().AddKey(ToTick(LastDisplayFrame), FMovieSceneDoubleValue(LastZ));
            Section->ExpandToFrame(ToTick(FirstDisplayFrame));
            Section->ExpandToFrame(ToTick(LastDisplayFrame));

            // The playback range is stored in TICK-resolution frames, not display frames. The
            // burst under test is driven by an explicit `frames` list, so the range is not what
            // picks the instants; it is set so Sequencer does not clamp a scrub that lands outside
            // a default range.
            MovieScene->SetPlaybackRange(TRange<FFrameNumber>(
                ToTick(FirstDisplayFrame), ToTick(LastDisplayFrame) + 1));
            MovieScene->Modify();
            return true;
        }

        // Declared FIRST so it is constructed first and snapshots the world before the spawn.
        FScopedEditorWorldActorGuard ActorGuard;
        ULevelSequence* Sequence = nullptr;
        FString SequencePath;
        FString ActorLabel;
        FString FailureReason;
        bool bReady = false;
    };

    // Two instants x one view. Small on purpose: every extra shot is a full viewport resize plus
    // an offscreen readback, and two instants is the minimum that can show the camera holding
    // across a time axis.
    TSharedPtr<FJsonObject> PWAnimSubjMakeBurstPayload(const FPWAnimSubjMovingFixture& Fixture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Fixture.GetActorLabel());
        Payload->SetStringField(TEXT("sequencePath"), Fixture.GetSequencePath());

        TArray<TSharedPtr<FJsonValue>> Frames;
        Frames.Add(MakeShared<FJsonValueNumber>(FPWAnimSubjMovingFixture::FirstDisplayFrame));
        Frames.Add(MakeShared<FJsonValueNumber>(FPWAnimSubjMovingFixture::LastDisplayFrame));
        Payload->SetArrayField(TEXT("frames"), Frames);

        Payload->SetNumberField(TEXT("width"), 128.0);
        Payload->SetNumberField(TEXT("height"), 128.0);
        return Payload;
    }

    void PWAnimSubjDeleteShotFiles(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("shots"), Shots) || !Shots)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& ShotValue : *Shots)
        {
            const TSharedPtr<FJsonObject>* ShotObj = nullptr;
            if (ShotValue.IsValid() && ShotValue->TryGetObject(ShotObj) && ShotObj)
            {
                FString Path;
                if ((*ShotObj)->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
                {
                    IFileManager::Get().Delete(*Path, /*RequireExists=*/false,
                        /*EvenReadOnly=*/true, /*Quiet=*/true);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Argument surface
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsSubjectParamTest,
    "PinWright.camera.animation_shots.SubjectParamIsDiscoverable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsSubjectParamTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("camera.animation_shots is registered"),
        IsHandlerRegistered(TEXT("camera.animation_shots")));

    // A parameter the body reads but the registration does not declare is unreachable from the
    // wire: the dispatcher rejects the whole call with UNKNOWN_PARAMS before the handler runs.
    // So declaration is the contract, not the body.
    const TCHAR* ExpectedParams[] = {
        TEXT("subject"),        // the subject descriptor this verb now normalises into
        TEXT("viewMode"),       // the scoped view-mode override this verb uniquely lacked
        TEXT("distribution"),   // ring | sphere, an INPUT to the `count` plan, not just a report
        TEXT("seed"),           // seeded jitter, so a set can be retaken exactly
        // Genuinely applicable here and nowhere in the preview-scene family: this verb draws the
        // LIVE Level Editor viewport, which has real AActors and therefore real editor icon
        // sprites over the subject. The preview verbs decline it because FPreviewScene registers
        // components with no actor, so there is no billboard to hide.
        TEXT("hideEditorSprites"),
        // Still declared, unchanged, and asserted here so a refactor that adds `subject` cannot
        // quietly retire the spellings every existing caller uses.
        TEXT("sequencePath"), TEXT("actorName"), TEXT("frames"), TEXT("frameCount"),
        TEXT("angles"), TEXT("count"), TEXT("views"), TEXT("width"), TEXT("height")
    };
    for (const TCHAR* ParamName : ExpectedParams)
    {
        TestNotNull(*FString::Printf(TEXT("camera.animation_shots declares a '%s' param"), ParamName),
            GetRegisteredParamSpec(TEXT("camera.animation_shots"), ParamName));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Decision 6: a capability a subject kind cannot support is a TYPED refusal
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsStaticMeshNoTimeAxisTest,
    "PinWright.camera.animation_shots.StaticMeshSubjectHasNoTimeAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsStaticMeshNoTimeAxisTest::RunTest(const FString& Parameters)
{
    // A static mesh has no time axis, so a burst of N instants of one is not a thing that exists.
    // What matters is WHICH refusal the caller gets: before `subject` was declared, the payload
    // died on the dispatcher's unknown-argument gate as UNKNOWN_PARAMS, which says nothing about
    // the missing time axis and sends the caller looking for a typo instead of a different verb.
    //
    // Deterministic: the kind is knowable from the request alone, so the refusal happens before
    // any actor is resolved, any sequence is loaded and any viewport is touched. Nothing
    // environmental can intervene, and nothing is left open by the rejected call.
    TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
    Subject->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    Subject->SetStringField(TEXT("path"), TEXT("/Engine/BasicShapes/Cube.Cube"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), Subject);
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/MCP_AnimShotsSubjAbsent/NoSuchSequence"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("camera.animation_shots responded"), Capture.bWasCalled);
    TestFalse(TEXT("a static-mesh subject is refused, not photographed"), Capture.bSuccess);
    TestEqual(TEXT("a subject kind with no time axis is refused as UNSUPPORTED_ASSET_EDITOR"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    // The second half of the same fact, asserted separately because it is the half the change
    // was made for: the old answer must be gone, not merely joined by a better one.
    TestNotEqual(TEXT("the refusal is no longer the dispatcher's UNKNOWN_PARAMS"),
        Capture.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    TestTrue(*FString::Printf(TEXT("the refusal names the missing time axis (got '%s')"),
                 *Capture.Message),
        Capture.Message.Contains(TEXT("time")));
    return true;
}

// ---------------------------------------------------------------------------
// The burst response shape
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsBurstShapeTest,
    "PinWright.camera.animation_shots.BurstResponseShapeSurvives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsBurstShapeTest::RunTest(const FString& Parameters)
{
    FPWAnimSubjMovingFixture Fixture;
    if (!Fixture.IsReady())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), Fixture.GetFailureReason());
        return true;
    }

    TSharedPtr<FJsonObject> Payload = PWAnimSubjMakeBurstPayload(Fixture);
    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    if (!Capture.bSuccess)
    {
        TestTrue(*FString::Printf(TEXT("a failed burst fails with a typed code (got '%s': %s)"),
                     *Capture.ErrorCode, *Capture.Message),
            PWAnimSubjIsEnvironmentalFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("burst-not-captured"),
            FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
        return true;
    }
    ON_SCOPE_EXIT { PWAnimSubjDeleteShotFiles(Capture.Result); };

    if (!TestTrue(TEXT("a successful burst returns a result object"), Capture.Result.IsValid()))
    {
        return false;
    }

    // ---- top-level keys, as literals ----
    //
    // Listed rather than spot-checked so a key dropped by a refactor NAMES ITSELF in the failure.
    // The first block is the shape this verb shipped with; the second is what the convergence
    // added. Both are asserted the same way because a caller cannot tell them apart.
    const TCHAR* RequiredTopLevel[] = {
        // shipped
        TEXT("shots"), TEXT("count"), TEXT("frames"), TEXT("frameCount"), TEXT("viewCount"),
        TEXT("framePlanSource"), TEXT("actorName"), TEXT("sequencePath"),
        TEXT("displayRate"), TEXT("tickResolution"), TEXT("updateMethod"), TEXT("forcedUpdate"),
        TEXT("opened"), TEXT("pausedPlayback"), TEXT("restoredPlayhead"),
        TEXT("width"), TEXT("height"), TEXT("resolutionSource"),
        TEXT("cameraDistance"), TEXT("framedCenter"), TEXT("framedRadius"), TEXT("blankShots"),
        TEXT("poseSampled"), TEXT("poseChanged"), TEXT("actorTranslationCm"),
        TEXT("viewport"),
        // added by the convergence
        TEXT("poseSet"), TEXT("shotDistribution"), TEXT("subject")
    };
    for (const TCHAR* Key : RequiredTopLevel)
    {
        TestTrue(*FString::Printf(TEXT("the burst response carries '%s'"), Key),
            Capture.Result->HasField(Key));
    }

    // resolutionSource speaks ONE vocabulary across every capture verb now
    // (CameraShotPlanUtils.h ResolveResolutionSource). This call passes width and height, so the
    // only correct answer is "caller" — and specifically NOT the retired "burstBudget".
    FString ResolutionSource;
    Capture.Result->TryGetStringField(TEXT("resolutionSource"), ResolutionSource);
    TestEqual(TEXT("explicit sizes are reported as caller-chosen"),
        ResolutionSource, FString(TEXT("caller")));

    // The set-level report the pose-list primitive owns. Its presence is what makes a silently
    // shortened set impossible to mistake for a set that was always that length.
    const TSharedPtr<FJsonObject>* PoseSet = PWAnimSubjObject(Capture.Result, TEXT("poseSet"));
    if (TestNotNull(TEXT("the burst publishes a poseSet block"), PoseSet) && PoseSet)
    {
        const TCHAR* RequiredPoseSet[] = {
            TEXT("posesRequested"), TEXT("posesCaptured"), TEXT("posesTruncated"),
            TEXT("maxPosesPerCall"), TEXT("warmupShotTaken"), TEXT("warmupShotDiscarded"),
            // Only present because this verb HAS a time axis and DOES supply bounds. A verb with
            // neither omits them, so asserting them here also pins that this verb has both.
            TEXT("subjectTimesApplied"), TEXT("posesFramingEvaluated"), TEXT("posesOutOfFrame")
        };
        for (const TCHAR* Key : RequiredPoseSet)
        {
            TestTrue(*FString::Printf(TEXT("poseSet carries '%s'"), Key),
                (*PoseSet)->HasField(Key));
        }
        // 24, not the primitive's default of 8. This verb has allowed a 24-shot burst since it
        // shipped and the conversion must not silently shorten it.
        TestEqual(TEXT("the burst keeps its own 24-shot bound, not the primitive's 8"),
            static_cast<int32>(PWAnimSubjNumber(*PoseSet, TEXT("maxPosesPerCall"), 0.0)), 24);
        // One application per captured pose, never one more: the warm-up frame reuses pose 0 and
        // must not re-drive the playhead.
        TestEqual(TEXT("the subject time was driven once per captured pose"),
            static_cast<int32>(PWAnimSubjNumber(*PoseSet, TEXT("subjectTimesApplied"), -1.0)),
            static_cast<int32>(PWAnimSubjNumber(*PoseSet, TEXT("posesCaptured"), -2.0)));
    }

    // The `subject` block: this verb always has a subject (the placed actor), whether the caller
    // spelled it `actorName` or `subject`.
    const TSharedPtr<FJsonObject>* SubjectInfo = PWAnimSubjObject(Capture.Result, TEXT("subject"));
    if (TestNotNull(TEXT("the burst publishes a subject block"), SubjectInfo) && SubjectInfo)
    {
        FString Kind;
        (*SubjectInfo)->TryGetStringField(TEXT("kind"), Kind);
        TestEqual(TEXT("a legacy actorName call normalises into an actor subject"),
            Kind, FString(TEXT("actor")));
        FString BoundsSource;
        (*SubjectInfo)->TryGetStringField(TEXT("boundsSource"), BoundsSource);
        // The union of the actor's bounds across every sampled instant, NOT one instant's bounds.
        // A posed skeletal mesh's bounds change every frame, so a camera solved from a single
        // instant would move under the subject.
        TestEqual(TEXT("burst bounds come from the sampled union, not one instant"),
            BoundsSource, FString(TEXT("sampledUnion")));
    }

    // ---- counts reconcile, and a complete set is not marked partial ----
    const int32 Count = static_cast<int32>(PWAnimSubjNumber(Capture.Result, TEXT("count"), -1.0));
    const int32 FrameCount =
        static_cast<int32>(PWAnimSubjNumber(Capture.Result, TEXT("frameCount"), -1.0));
    const int32 ViewCount =
        static_cast<int32>(PWAnimSubjNumber(Capture.Result, TEXT("viewCount"), -1.0));
    TestEqual(TEXT("two explicit frames produce two instants"), FrameCount, 2);
    TestEqual(TEXT("the default view plan is one angle"), ViewCount, 1);
    TestEqual(TEXT("shots = instants x angles"), Count, FrameCount * ViewCount);
    // ABSENCE, both keys. A complete burst has nothing partial to say, and a `partial: false`
    // emitted on every success would be the fourteenth absence assertion nobody knew about.
    TestFalse(TEXT("a complete burst carries no 'partial' flag"),
        Capture.Result->HasField(TEXT("partial")));
    TestFalse(TEXT("a complete burst carries no 'captureError' block"),
        Capture.Result->HasField(TEXT("captureError")));

    // ---- per-shot keys, as literals ----
    const int32 TopWidth = static_cast<int32>(PWAnimSubjNumber(Capture.Result, TEXT("width"), -1.0));
    const int32 TopHeight = static_cast<int32>(PWAnimSubjNumber(Capture.Result, TEXT("height"), -1.0));
    TestEqual(TEXT("the caller's width is what the response reports"), TopWidth, 128);
    TestEqual(TEXT("the caller's height is what the response reports"), TopHeight, 128);

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (TestTrue(TEXT("a successful burst returns a shots array"),
            Capture.Result->TryGetArrayField(TEXT("shots"), Shots)) && Shots)
    {
        TestEqual(TEXT("the shots array length equals count"), Shots->Num(), Count);
        int32 ShotIndex = 0;
        for (const TSharedPtr<FJsonValue>& ShotValue : *Shots)
        {
            const TSharedPtr<FJsonObject>* ShotObj = nullptr;
            if (!ShotValue.IsValid() || !ShotValue->TryGetObject(ShotObj) || !ShotObj)
            {
                AddError(FString::Printf(TEXT("shot %d is not an object"), ShotIndex));
                ++ShotIndex;
                continue;
            }
            const TCHAR* RequiredShotKeys[] = {
                // the burst's own per-shot identity
                TEXT("frameIndex"), TEXT("frame"), TEXT("time"), TEXT("viewIndex"), TEXT("angle"),
                // the shared serializer (CameraShotPlanUtils AddShotFields)
                TEXT("path"), TEXT("filename"), TEXT("width"), TEXT("height"), TEXT("sizeBytes"),
                TEXT("projectionMode"), TEXT("cameraLocation"), TEXT("cameraRotation"),
                TEXT("renderer"), TEXT("mimeType"), TEXT("imageStats"), TEXT("blank")
            };
            for (const TCHAR* Key : RequiredShotKeys)
            {
                TestTrue(*FString::Printf(TEXT("shot %d carries '%s'"), ShotIndex, Key),
                    (*ShotObj)->HasField(Key));
            }

            // THE capture-size rule, asserted per shot rather than trusted from one top-level
            // number: a capture size that VARIES within an editor session trips
            // `Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in
            // FViewport::GetHitProxy, an incident that cost 66 actors and 125 emitters of
            // unsaved level state. The verb resolves resolution ONCE for exactly this reason, and
            // this is the assertion that notices if it ever stops.
            TestEqual(*FString::Printf(TEXT("shot %d was captured at the set's width"), ShotIndex),
                static_cast<int32>(PWAnimSubjNumber(*ShotObj, TEXT("width"), -1.0)), TopWidth);
            TestEqual(*FString::Printf(TEXT("shot %d was captured at the set's height"), ShotIndex),
                static_cast<int32>(PWAnimSubjNumber(*ShotObj, TEXT("height"), -1.0)), TopHeight);
            ++ShotIndex;
        }
    }

    // ---- the per-instant pose evidence, which is the verb's whole reason to exist ----
    const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
    if (TestTrue(TEXT("a successful burst returns a frames array"),
            Capture.Result->TryGetArrayField(TEXT("frames"), Frames)) && Frames)
    {
        TestEqual(TEXT("one frame entry per sampled instant"), Frames->Num(), FrameCount);
        const TSharedPtr<FJsonObject>* Second = nullptr;
        if (Frames->Num() > 1 && (*Frames)[1].IsValid() &&
            (*Frames)[1]->TryGetObject(Second) && Second)
        {
            for (const TCHAR* Key : {TEXT("index"), TEXT("frame"), TEXT("time"),
                     TEXT("poseSampled"), TEXT("poseDeltaFromPrevious"), TEXT("poseDeltaFromFirst")})
            {
                TestTrue(*FString::Printf(TEXT("frame 1 carries '%s'"), Key),
                    (*Second)->HasField(Key));
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// One camera for the whole burst
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsOneCameraTest,
    "PinWright.camera.animation_shots.BurstHoldsOneCameraAcrossInstants",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsOneCameraTest::RunTest(const FString& Parameters)
{
    FPWAnimSubjMovingFixture Fixture;
    if (!Fixture.IsReady())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), Fixture.GetFailureReason());
        return true;
    }

    TSharedPtr<FJsonObject> Payload = PWAnimSubjMakeBurstPayload(Fixture);
    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    if (!Capture.bSuccess)
    {
        TestTrue(*FString::Printf(TEXT("a failed burst fails with a typed code (got '%s': %s)"),
                     *Capture.ErrorCode, *Capture.Message),
            PWAnimSubjIsEnvironmentalFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("burst-not-captured"),
            FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
        return true;
    }
    ON_SCOPE_EXIT { PWAnimSubjDeleteShotFiles(Capture.Result); };

    // ---- PRECONDITION FIRST: the subject actually moved between the two instants ----
    //
    // Without this the whole test is unable to fail. A fixture that never moves has one bounds
    // union equal to its single-instant bounds, so the camera would be identical for a reason
    // that has nothing to do with the rule under test, and the assertion below would pass on a
    // verb that re-solved the camera every frame.
    const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
    if (!Capture.Result.IsValid() ||
        !Capture.Result->TryGetArrayField(TEXT("frames"), Frames) || !Frames ||
        Frames->Num() < 2)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-two-instants"),
            TEXT("the burst returned fewer than two sampled instants"));
        return true;
    }
    const TSharedPtr<FJsonObject>* SecondFrame = nullptr;
    double MovedCm = 0.0;
    if ((*Frames)[1].IsValid() && (*Frames)[1]->TryGetObject(SecondFrame) && SecondFrame)
    {
        const TSharedPtr<FJsonObject>* FromFirst =
            PWAnimSubjObject(*SecondFrame, TEXT("poseDeltaFromFirst"));
        if (FromFirst)
        {
            MovedCm = PWAnimSubjNumber(*FromFirst, TEXT("componentTranslationCm"), 0.0);
        }
    }
    if (MovedCm <= 0.0)
    {
        // Not an assertion failure: on a host where Sequencer never evaluated the transform track
        // the fixture is what is broken, not the verb. Announced so the run cannot read clean.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-did-not-move"),
            FString::Printf(TEXT("componentTranslationCm=%.6f between the two instants; the "
                                 "one-camera assertion cannot fail against a static subject"),
                MovedCm));
        return true;
    }
    TestTrue(TEXT("PRECONDITION: the subject moved between the two sampled instants"),
        MovedCm > 0.0);
    // The transform track moves it 1200 cm; anything an order of magnitude smaller means the
    // track did not drive it and the number came from somewhere else.
    TestTrue(*FString::Printf(TEXT("the subject moved the keyed distance (got %.2f cm)"), MovedCm),
        MovedCm > 100.0);

    // ---- THE assertion: one camera, held across every instant at a fixed view index ----
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("shots"), Shots) || !Shots || Shots->Num() < 2)
    {
        AddError(TEXT("a two-instant burst returned fewer than two shots"));
        return true;
    }

    // Both shots are view index 0 of the default single-angle plan, so any difference in
    // cameraLocation is the camera having followed the subject. cameraLocation is MEASURED off
    // the client after the camera was applied, not echoed from the request, so this cannot pass
    // on a verb that asked for one camera and got another.
    const TSharedPtr<FJsonObject>* FirstShot = nullptr;
    const TSharedPtr<FJsonObject>* SecondShot = nullptr;
    (*Shots)[0]->TryGetObject(FirstShot);
    (*Shots)[1]->TryGetObject(SecondShot);
    if (!FirstShot || !SecondShot)
    {
        AddError(TEXT("shots are not objects"));
        return true;
    }
    TestEqual(TEXT("both shots are the same view index"),
        static_cast<int32>(PWAnimSubjNumber(*FirstShot, TEXT("viewIndex"), -1.0)),
        static_cast<int32>(PWAnimSubjNumber(*SecondShot, TEXT("viewIndex"), -2.0)));
    // TestNotEqual has no numeric overload in UE 5.8 (AutomationTest.h:2004-2012 is string-only),
    // so the inequality is asserted through TestTrue rather than silently converting to strings.
    TestTrue(TEXT("the two shots are different instants"),
        static_cast<int32>(PWAnimSubjNumber(*FirstShot, TEXT("frameIndex"), -1.0)) !=
        static_cast<int32>(PWAnimSubjNumber(*SecondShot, TEXT("frameIndex"), -2.0)));

    const TSharedPtr<FJsonObject>* FirstLoc = PWAnimSubjObject(*FirstShot, TEXT("cameraLocation"));
    const TSharedPtr<FJsonObject>* SecondLoc = PWAnimSubjObject(*SecondShot, TEXT("cameraLocation"));
    if (TestNotNull(TEXT("shot 0 reports a measured camera location"), FirstLoc) &&
        TestNotNull(TEXT("shot 1 reports a measured camera location"), SecondLoc) &&
        FirstLoc && SecondLoc)
    {
        for (const TCHAR* Axis : {TEXT("x"), TEXT("y"), TEXT("z")})
        {
            const double A = PWAnimSubjNumber(*FirstLoc, Axis, 0.0);
            const double B = PWAnimSubjNumber(*SecondLoc, Axis, 1.0);
            // Tolerance 0.0, explicitly: the double overload of TestEqual defaults to
            // UE_KINDA_SMALL_NUMBER, and "the camera nearly held" is not the contract. Both
            // numbers come from the same solved centre and distance, so they are the same bits or
            // the camera was re-solved.
            TestEqual(*FString::Printf(
                          TEXT("the camera's %s is identical across instants (%.6f vs %.6f)"),
                          Axis, A, B),
                A, B, 0.0);
        }
    }

    // The framing solve is a property of the SET, so both of these are single top-level values
    // and neither may vary per instant. Asserting them alongside the camera is what distinguishes
    // "the camera happened to land in the same place" from "the camera was solved once".
    TestTrue(TEXT("the burst reports one camera distance for the whole set"),
        Capture.Result->HasField(TEXT("cameraDistance")));
    TestTrue(TEXT("the burst reports one framed radius for the whole set"),
        Capture.Result->HasField(TEXT("framedRadius")));
    return true;
}
