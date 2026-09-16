// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Sequencer domain handlers
// Covers: SequenceHandler.cpp (32 handlers) and SequencerHandler.cpp (5 handlers)
#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

#include "LevelSequence.h"
#include "Misc/Guid.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "GameFramework/Actor.h"
#include "Camera/CameraActor.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Misc/ScopeExit.h"

// ============================================================================
// Registered in-memory LevelSequence fixture (sequencer-test-local)
// ============================================================================
// Several handlers here (sequencer.add_camera, sequence.add_keyframe, ...) resolve
// their target sequence via UEditorAssetLibrary::LoadAsset, which goes through the
// AssetRegistry (GetAssetByObjectPath). A bare NewObject in a transient package is
// never registered, so LoadAsset returns null and the handler rejects with
// INVALID_SEQUENCE before reaching the branch under test. This RAII guard builds a
// LevelSequence in a real /Game package and registers it so LoadAsset can resolve it
// by path. Notes baked into the construction:
//   - No RF_Transient: UObject::IsAsset() (enforced by LoadAsset) rejects transient
//     objects. The object stays in-memory only (the /Game package is never saved).
//   - AddToRoot keeps it alive across the test; the destructor unregisters
//     (AssetDeleted) and unroots it, replacing the per-test ON_SCOPE_EXIT teardown.
// Prefer this over re-inlining the CreatePackage/NewObject/Initialize/AddToRoot/
// AssetCreated block (it was previously copy-pasted across these tests).
// Anonymous namespace: keeps the type TU-local so a Unity-merged build can't hit an
// ODR collision with a same-named helper from another translation unit in the blob.
namespace
{
struct FScopedRegisteredSequence
{
    explicit FScopedRegisteredSequence(const TCHAR* Prefix)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString PackagePath = FString::Printf(TEXT("/Game/%s_%s"), Prefix, *Suffix);
        const FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix, *Suffix);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return;
        }
        Sequence = NewObject<ULevelSequence>(Package, FName(*AssetName), RF_Public | RF_Standalone);
        if (!Sequence)
        {
            return;
        }
        Sequence->Initialize();
        Sequence->AddToRoot();
        FAssetRegistryModule::AssetCreated(Sequence);
        ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    }

    ~FScopedRegisteredSequence()
    {
        if (Sequence)
        {
            // Clear the package dirty flag before teardown. The mutating handlers under test
            // (sequencer.add_camera, sequence.add_keyframe, ...) dirty this non-transient /Game
            // package; a later editor-wide save-all in the suite (editor.save_all
            // tests) would otherwise flush it to the host Content tree as GUID-named .uasset
            // litter. De-dirtying here leaves nothing for save-all to write. B-tests-leak-host-content.
            if (UPackage* Package = Sequence->GetOutermost())
            {
                Package->SetDirtyFlag(false);
            }
            FAssetRegistryModule::AssetDeleted(Sequence);
            Sequence->RemoveFromRoot();
        }
    }

    FScopedRegisteredSequence(const FScopedRegisteredSequence&) = delete;
    FScopedRegisteredSequence& operator=(const FScopedRegisteredSequence&) = delete;

    bool IsValid() const { return Sequence != nullptr; }

    ULevelSequence* Sequence = nullptr;
    FString ObjectPath;
};

// Required-param gate, observed where it actually lives.
// The `*MissingRequiredParamTest` cases below call InvokeHandler(), which invokes
// Reg.Func(Ctx) directly and never runs FRpcDispatcher::ValidateHandlerParams
// (RpcDispatcher.cpp) — the code that enforces RPC_PARAM_REQ. They also assert nothing
// about the response, only that the method is registered, so flipping a required param
// to RPC_PARAM_OPT changes nothing they observe. This helper routes a request through a
// real dispatcher so the MISSING_REQUIRED_PARAM contract is observable; it is the shape
// the rest of those tests should be converted to. Validation rejects before the handler
// body runs, so no asset is created by a missing-param dispatch.
// Named FSequencerDispatcherValidationResult (not the generic name used by the sibling
// helper in Tests/Assets/TestMaterialHandlers.cpp) so a Unity-merged build cannot hit an
// ODR collision between the two anonymous namespaces.
struct FSequencerDispatcherValidationResult
{
    bool bCompletionFired = false;
    bool bSuccess = false;
    FString Message;
    FString ErrorCode;
};

FSequencerDispatcherValidationResult DispatchSequencerRequestViaDispatcher(
    const FString& RequestId, const FString& MethodName, const TSharedPtr<FJsonObject>& Payload)
{
    FSequencerDispatcherValidationResult Result;
    FRpcDispatcher Dispatcher;
    // Initialize BEFORE draining so the bridge lambdas capture a live sink.
    Dispatcher.Initialize(FResponseSink(
        [&Result](const FString&, bool bInSuccess, const FString& InMessage,
                  const TSharedPtr<FJsonObject>&, const FString& InErrorCode)
        {
            Result.bCompletionFired = true;
            Result.bSuccess = bInSuccess;
            Result.Message = InMessage;
            Result.ErrorCode = InErrorCode;
        }));
    Dispatcher.DrainAutoRegistrations(nullptr);
    Dispatcher.ProcessRequest(RequestId, MethodName, Payload.IsValid() ? Payload : MakeShared<FJsonObject>());
    return Result;
}

}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCreateRejectsMissingNameViaDispatcherTest,
    "PinWright.sequencer.create.RejectsMissingNameViaDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceCreateRejectsMissingNameViaDispatcherTest::RunTest(const FString& Parameters)
{
    // sequencer.create declares name as RPC_PARAM_REQ (SequenceHandler.cpp). The gate that
    // enforces it is in the dispatcher, so an empty payload must come back
    // MISSING_REQUIRED_PARAM without the handler body ever running (nothing is created).
    // Demote the param to RPC_PARAM_OPT and this goes red; FSequenceCreateMissingRequiredParamTest
    // below does not.
    const FSequencerDispatcherValidationResult Result = DispatchSequencerRequestViaDispatcher(
        TEXT("req-seq-create-missing"), TEXT("sequencer.create"), MakeShared<FJsonObject>());

    TestTrue(TEXT("completion fired"), Result.bCompletionFired);
    TestEqual(TEXT("missing name rejected by the dispatcher"),
        Result.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    return true;
}

// ============================================================================
// FScopedRegisteredSequence cleanup contract (B-tests-leak-host-content)
// ============================================================================
// The fixture builds a non-transient /Game package so the handlers' LoadAsset can
// resolve the sequence by path. The mutating handlers under test dirty that package,
// and an editor-wide save-all later in the suite would flush it to the host Content
// tree as GUID-named .uasset litter. The fixture destructor must leave no dirty
// package behind. This test dirties the package (as a mutating handler does) and
// asserts the destructor de-dirtied it — it fails if the destructor's SetDirtyFlag(false)
// is reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerFixtureLeavesNoDirtyPackageTest,
    "PinWright.sequencer.fixture.LeavesNoDirtyPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerFixtureLeavesNoDirtyPackageTest::RunTest(const FString& Parameters)
{
    FString PkgName;
    {
        FScopedRegisteredSequence Fixture(TEXT("SeqLeakRegression"));
        if (!TestTrue(TEXT("fixture built a registered sequence"), Fixture.IsValid()))
        {
            return true;
        }
        UPackage* Pkg = Fixture.Sequence->GetOutermost();
        TestNotNull(TEXT("fixture sequence has an outer package"), Pkg);
        if (!Pkg)
        {
            return true;
        }
        PkgName = Pkg->GetName();
        // Mimic a mutating handler (sequencer.add_camera, ...) that dirties the /Game package.
        Pkg->SetDirtyFlag(true);
        TestTrue(TEXT("package is dirty while the fixture is alive"), Pkg->IsDirty());
    }
    // Fixture destroyed: the package must not remain dirty (else a save-all leaks it to disk).
    // It may still linger in memory (GC is deferred) or be gone — either is fine as long as
    // nothing dirty survives for save-all to flush.
    UPackage* Remaining = FindPackage(nullptr, *PkgName);
    TestTrue(TEXT("fixture destructor left no dirty /Game package for save-all to flush"),
        Remaining == nullptr || !Remaining->IsDirty());
    return true;
}

// ============================================================================
// sequencer.create
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCreateValidParamsNoCrashTest,
    "PinWright.sequencer.create.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceCreateValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestSeq"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Sequences"));
    TestTrue(TEXT("sequencer.create handler found"), InvokeHandler(TEXT("sequencer.create"), Payload));
    CleanupTestAsset(TEXT("/Game/Sequences/TestSeq"));
    return true;
}

// ============================================================================
// sequencer.set_display_rate
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetDisplayRateValidParamsNoCrashTest,
    "PinWright.sequencer.set_display_rate.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetDisplayRateValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("frameRate"), TEXT("30fps"));
    TestTrue(TEXT("sequencer.set_display_rate handler found"), InvokeHandler(TEXT("sequencer.set_display_rate"), Payload));
    return true;
}

// ============================================================================
// sequencer.set_properties  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetPropertiesValidParamsNoCrashTest,
    "PinWright.sequencer.set_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetPropertiesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Sequences/MySeq"));
    Payload->SetNumberField(TEXT("frameRate"), 30.0);
    TestTrue(TEXT("sequencer.set_properties handler found"), InvokeHandler(TEXT("sequencer.set_properties"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_camera  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddCameraValidParamsNoCrashTest,
    "PinWright.sequencer.add_camera.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddCameraValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Omit path here to avoid hard editor asset-load errors in unit mode.
    TestTrue(TEXT("sequencer.add_camera handler found"), InvokeHandler(TEXT("sequencer.add_camera"), Payload));
    return true;
}

// Regression test for E-sequencer-add-camera-no-actor-path:
// sequencer.add_camera used to return only {bindingGuid, success, actorLabel}, so its
// natural successor sequencer.add_camera_track (which REQUIRES cameraActorPath, resolved
// via LoadObject<ACameraActor>) could not chain — the only recovery was a cross-namespace
// actor.find_by_name detour keyed on the fixed non-unique label "SequenceCamera". The fix
// emits the spawned camera's loadable actor object path. This asserts add_camera now returns
// a cameraActorPath that resolves to a real ACameraActor exactly the way add_camera_track's
// consumer resolves it (LoadObject<ACameraActor>), plus a unique engine actor name. It fails
// if the path field is removed (revert) or stops resolving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddCameraReturnsLoadableActorPathTest,
    "PinWright.sequencer.add_camera.ReturnsLoadableActorPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddCameraReturnsLoadableActorPathTest::RunTest(const FString& Parameters)
{
    // add_camera spawns into the active editor world; without GEditor there is nowhere to
    // spawn and the handler returns NOT_AVAILABLE. Skip rather than assert in that case.
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping add_camera path round-trip."));
        return true;
    }

    // add_camera spawns an ACameraActor into the persistent editor world (the host startup
    // map, e.g. L_Core), dirtying its level package; Resolved->Destroy() below re-dirties it.
    // Guard the spawn so the camera is destroyed and the level package's dirty flag is
    // restored on scope exit — otherwise a later editor-wide save-all in the suite flushes
    // the dirtied L_Core.umap into the host Content tree. B-tests-leak-host-content.
    FScopedEditorWorldActorGuard WorldGuard;

    // Registered in-memory sequence so the handler's LoadAsset can resolve it by path
    // (see FScopedRegisteredSequence for why /Game + non-transient + AssetCreated).
    FScopedRegisteredSequence Fixture(TEXT("SequenceAddCameraPath"));
    if (!TestTrue(TEXT("registered sequence fixture created"), Fixture.IsValid()))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), Fixture.ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.add_camera handler found"),
        InvokeHandlerWithCapture(TEXT("sequencer.add_camera"), Payload, Capture));
    TestTrue(TEXT("sequencer.add_camera succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // The core regression assertion: the spawned camera's loadable actor path must be emitted,
    // not just the (non-unique) actorLabel.
    FString CameraActorPath;
    TestTrue(TEXT("cameraActorPath field present"),
        Capture.Result->TryGetStringField(TEXT("cameraActorPath"), CameraActorPath));
    TestFalse(TEXT("cameraActorPath is not empty"), CameraActorPath.IsEmpty());

    // The returned actorLabel is provably NOT a usable path — it must differ from the real path.
    FString ActorLabel;
    Capture.Result->TryGetStringField(TEXT("actorLabel"), ActorLabel);
    TestNotEqual(TEXT("cameraActorPath is not just the (non-unique) actorLabel"),
        CameraActorPath, ActorLabel);

    // Unique engine-assigned actor name accompanies the path (disambiguates the fixed label).
    FString ActorName;
    TestTrue(TEXT("actorName field present"),
        Capture.Result->TryGetStringField(TEXT("actorName"), ActorName));
    TestFalse(TEXT("actorName is not empty"), ActorName.IsEmpty());

    // Round-trip: the consumer sequencer.add_camera_track resolves cameraActorPath via
    // LoadObject<ACameraActor>(nullptr, *CameraActorPath). Prove the producer's path is
    // consumable the exact same way — this is what closes the producer->consumer gap.
    ACameraActor* Resolved = LoadObject<ACameraActor>(nullptr, *CameraActorPath);
    TestNotNull(TEXT("cameraActorPath resolves to an ACameraActor (chainable into add_camera_track)"),
        Resolved);

    if (Resolved)
    {
        Resolved->Destroy();
    }
    return true;
}

// Differential regression for the L_Core.umap leak (B-tests-leak-host-content):
// sequencer.add_camera spawns an ACameraActor into the persistent editor world, dirtying its
// level package; a later editor-wide save-all would then flush the host startup map
// (L_Core.umap) to disk. FScopedEditorWorldActorGuard must destroy the spawned camera and
// restore the level's dirty flag on scope exit. This forces the level clean, runs a guarded
// add_camera, and asserts the level is left clean afterwards — it goes red if the guard is
// dropped from the add_camera path (the spawned camera then leaves the level dirty).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddCameraLeavesLevelCleanTest,
    "PinWright.sequencer.add_camera.LeavesLevelClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddCameraLeavesLevelCleanTest::RunTest(const FString& Parameters)
{
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!EditorWorld || !EditorWorld->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping add_camera level-clean check."));
        return true;
    }
    UPackage* LevelPackage = EditorWorld->PersistentLevel->GetOutermost();
    if (!TestNotNull(TEXT("persistent level has an outer package"), LevelPackage))
    {
        return true;
    }

    // Start from a known-clean level so the post-scope assertion is a real differential: the
    // guarded add_camera below spawns (and the guard destroys) a camera, and the guard must
    // restore this cleared flag. Without the guard the camera leaves the level dirty.
    LevelPackage->SetDirtyFlag(false);

    {
        FScopedEditorWorldActorGuard WorldGuard;
        FScopedRegisteredSequence Fixture(TEXT("SeqAddCamLevelClean"));
        if (!TestTrue(TEXT("registered sequence fixture created"), Fixture.IsValid()))
        {
            return true;
        }
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), Fixture.ObjectPath);
        FTestResponseCapture Capture;
        TestTrue(TEXT("sequencer.add_camera handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_camera"), Payload, Capture));
        // The spawned camera dirties the persistent level while the guard is in scope; the
        // handler must have actually spawned for this to be a live differential.
        TestTrue(TEXT("sequencer.add_camera succeeded"), Capture.bSuccess);
    }

    // The guard restored the level's dirty flag: nothing dirty survives for a later
    // editor-wide save-all to flush the host startup map to disk.
    TestFalse(TEXT("add_camera left no dirty persistent-level package for save-all to flush"),
        LevelPackage->IsDirty());
    return true;
}

// ============================================================================
// sequencer.play  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePlayValidParamsNoCrashTest,
    "PinWright.sequencer.play.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencePlayValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Sequences/MySeq"));
    TestTrue(TEXT("sequencer.play handler found"), InvokeHandler(TEXT("sequencer.play"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_actor
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddActorValidParamsNoCrashTest,
    "PinWright.sequencer.add_actor.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddActorValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("BP_TestActor"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Sequences/MySeq"));
    TestTrue(TEXT("sequencer.add_actor handler found"), InvokeHandler(TEXT("sequencer.add_actor"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_actors
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddActorsValidParamsNoCrashTest,
    "PinWright.sequencer.add_actors.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddActorsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ActorNames;
    ActorNames.Add(MakeShared<FJsonValueString>(TEXT("ActorA")));
    ActorNames.Add(MakeShared<FJsonValueString>(TEXT("ActorB")));
    Payload->SetArrayField(TEXT("actorNames"), ActorNames);
    TestTrue(TEXT("sequencer.add_actors handler found"), InvokeHandler(TEXT("sequencer.add_actors"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_spawnable_from_class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddSpawnableFromClassValidParamsNoCrashTest,
    "PinWright.sequencer.add_spawnable_from_class.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddSpawnableFromClassValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("className"), TEXT("StaticMeshActor"));
    TestTrue(TEXT("sequencer.add_spawnable_from_class handler found"),
        InvokeHandler(TEXT("sequencer.add_spawnable_from_class"), Payload));
    return true;
}

// ============================================================================
// sequencer.remove_actors
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRemoveActorsValidParamsNoCrashTest,
    "PinWright.sequencer.remove_actors.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceRemoveActorsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ActorNames;
    ActorNames.Add(MakeShared<FJsonValueString>(TEXT("ActorA")));
    Payload->SetArrayField(TEXT("actorNames"), ActorNames);
    TestTrue(TEXT("sequencer.remove_actors handler found"), InvokeHandler(TEXT("sequencer.remove_actors"), Payload));
    return true;
}

// ============================================================================
// sequencer.get_bindings  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceGetBindingsValidParamsNoCrashTest,
    "PinWright.sequencer.get_bindings.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceGetBindingsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Sequences/MySeq"));
    TestTrue(TEXT("sequencer.get_bindings handler found"), InvokeHandler(TEXT("sequencer.get_bindings"), Payload));
    return true;
}

// ============================================================================
// sequencer.get_properties  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceGetPropertiesValidParamsNoCrashTest,
    "PinWright.sequencer.get_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceGetPropertiesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Sequences/MySeq"));
    TestTrue(TEXT("sequencer.get_properties handler found"), InvokeHandler(TEXT("sequencer.get_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceGetPropertiesIncludesMovieSceneMetadataTest,
    "PinWright.sequencer.get_properties.IncludesMovieSceneMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceGetPropertiesIncludesMovieSceneMetadataTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/SequenceGetProperties_%s"), *Suffix);
    const FString AssetName = FString::Printf(TEXT("SequenceGetProperties_%s"), *Suffix);
    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    ULevelSequence* Sequence = NewObject<ULevelSequence>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("LevelSequence created"), Sequence))
    {
        return true;
    }
    Sequence->Initialize();

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene created"), MovieScene))
    {
        return true;
    }
    MovieScene->SetTickResolutionDirectly(FFrameRate(48000, 1001));

    // Populate bindings so the three count fields are not all zero. On an empty
    // MovieScene bindingCount / spawnableCount / possessableCount are 0, 0, 0 — every
    // assertion below then reads 0 == 0 and cannot tell the three fields apart, nor a
    // hardcoded 0 from a real read. Two possessables make bindingCount and
    // possessableCount 2 while spawnableCount stays 0, so a field that reports the
    // wrong quantity (or a constant) goes red.
    MovieScene->AddPossessable(TEXT("GetPropsPossessableA"), AActor::StaticClass());
    MovieScene->AddPossessable(TEXT("GetPropsPossessableB"), AActor::StaticClass());

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.get_properties handler found"),
        InvokeHandlerWithCapture(TEXT("sequencer.get_properties"), Payload, Capture));
    TestTrue(TEXT("sequencer.get_properties succeeded"), Capture.bSuccess);
    TestTrue(TEXT("sequencer.get_properties returned result"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* TickResolutionObj = nullptr;
    if (!TestTrue(TEXT("tickResolution object exists"),
        Capture.Result->TryGetObjectField(TEXT("tickResolution"), TickResolutionObj)))
    {
        return true;
    }

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    TestEqual(TEXT("tickResolution.numerator matches MovieScene"),
        (*TickResolutionObj)->GetNumberField(TEXT("numerator")), static_cast<double>(TickResolution.Numerator));
    TestEqual(TEXT("tickResolution.denominator matches MovieScene"),
        (*TickResolutionObj)->GetNumberField(TEXT("denominator")), static_cast<double>(TickResolution.Denominator));

    double BindingCount = -1.0;
    TestTrue(TEXT("bindingCount field present"), Capture.Result->TryGetNumberField(TEXT("bindingCount"), BindingCount));
    // Call the const overload; non-const GetBindings() is deprecated (C4996) in UE 5.7.
    TestEqual(TEXT("bindingCount matches MovieScene"), BindingCount,
        static_cast<double>(static_cast<const UMovieScene*>(MovieScene)->GetBindings().Num()));

    double SpawnableCount = -1.0;
    TestTrue(TEXT("spawnableCount field present"), Capture.Result->TryGetNumberField(TEXT("spawnableCount"), SpawnableCount));
    TestEqual(TEXT("spawnableCount matches MovieScene"), SpawnableCount, static_cast<double>(MovieScene->GetSpawnableCount()));

    double PossessableCount = -1.0;
    TestTrue(TEXT("possessableCount field present"), Capture.Result->TryGetNumberField(TEXT("possessableCount"), PossessableCount));
    TestEqual(TEXT("possessableCount matches MovieScene"), PossessableCount, static_cast<double>(MovieScene->GetPossessableCount()));

    // Pin the expected quantities directly, not only against the same MovieScene the
    // handler read: the two possessables added above are the independent reference.
    TestEqual(TEXT("bindingCount reports the two possessables"), BindingCount, 2.0);
    TestEqual(TEXT("possessableCount reports the two possessables"), PossessableCount, 2.0);
    TestEqual(TEXT("spawnableCount is not standing in for the possessables"), SpawnableCount, 0.0);

    return true;
}

// ============================================================================
// sequencer.set_playback_speed  (all-optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetPlaybackSpeedValidParamsNoCrashTest,
    "PinWright.sequencer.set_playback_speed.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetPlaybackSpeedValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("speed"), 2.0);
    TestTrue(TEXT("sequencer.set_playback_speed handler found"),
        InvokeHandler(TEXT("sequencer.set_playback_speed"), Payload));
    return true;
}

// ============================================================================
// sequencer.pause  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePauseValidParamsNoCrashTest,
    "PinWright.sequencer.pause.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencePauseValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("sequencer.pause handler found"), InvokeHandler(TEXT("sequencer.pause"), Payload));
    return true;
}

// ============================================================================
// sequencer.stop  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceStopValidParamsNoCrashTest,
    "PinWright.sequencer.stop.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceStopValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("sequencer.stop handler found"), InvokeHandler(TEXT("sequencer.stop"), Payload));
    return true;
}

// ============================================================================
// sequence.add_keyframe  (REQ: frame)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddKeyframeValidParamsNoCrashTest,
    "PinWright.sequence.add_keyframe.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddKeyframeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("MySphere"));
    Payload->SetStringField(TEXT("property"), TEXT("Location"));
    Payload->SetNumberField(TEXT("frame"), 30.0);
    TestTrue(TEXT("sequence.add_keyframe handler found"), InvokeHandler(TEXT("sequence.add_keyframe"), Payload));
    return true;
}

// Regression test for B-sequence-add-keyframe-location-property-rejected:
// property="Location" with a {x,y,z} vector value is advertised by the param schema
// and the handler's own INVALID_ARGUMENT example, but used to be unconditionally
// rejected with UNSUPPORTED_PROPERTY (the generic branch only accepted number/bool).
// This asserts the documented call now succeeds AND writes keys onto the transform
// track's location double channels (0-2). It fails if the Location branch is reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddKeyframeLocationVectorWritesChannelsTest,
    "PinWright.sequence.add_keyframe.LocationVectorWritesChannels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddKeyframeLocationVectorWritesChannelsTest::RunTest(const FString& Parameters)
{
    // Registered in-memory sequence so the handler's LoadAsset can resolve it by path
    // (see FScopedRegisteredSequence for why /Game + non-transient + AssetCreated).
    FScopedRegisteredSequence Fixture(TEXT("SequenceAddKeyframeLoc"));
    if (!TestTrue(TEXT("registered sequence fixture created"), Fixture.IsValid()))
    {
        return true;
    }
    ULevelSequence* Sequence = Fixture.Sequence;

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene created"), MovieScene))
    {
        return true;
    }

    // A possessable binding the handler can resolve via bindingId -> FindBinding.
    const FGuid BindingGuid = MovieScene->AddPossessable(TEXT("KeyframeTarget"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding created"), BindingGuid.IsValid()))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), Fixture.ObjectPath);
    Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString());
    Payload->SetStringField(TEXT("property"), TEXT("Location"));
    Payload->SetNumberField(TEXT("frame"), 0.0);
    TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
    Value->SetNumberField(TEXT("x"), -800.0);
    Value->SetNumberField(TEXT("y"), 400.0);
    Value->SetNumberField(TEXT("z"), 200.0);
    Payload->SetObjectField(TEXT("value"), Value);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequence.add_keyframe handler found"),
        InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), Payload, Capture));
    // The core regression assertion: the documented Location call must NOT be rejected.
    TestTrue(TEXT("sequence.add_keyframe property=Location succeeded"), Capture.bSuccess);
    TestNotEqual(TEXT("error code is not UNSUPPORTED_PROPERTY"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_PROPERTY")));
    if (!Capture.bSuccess)
    {
        return true;
    }

    // Verify the keys landed on the transform track's location channels (0-2).
    UMovieScene3DTransformTrack* Track =
        MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
    if (!TestNotNull(TEXT("transform track was created/found"), Track))
    {
        return true;
    }
    if (!TestTrue(TEXT("transform track has a section"), Track->GetAllSections().Num() > 0))
    {
        return true;
    }
    UMovieScene3DTransformSection* Section =
        Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[0]);
    if (!TestNotNull(TEXT("section is a transform section"), Section))
    {
        return true;
    }

    TArrayView<FMovieSceneDoubleChannel*> Channels =
        Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    if (!TestTrue(TEXT("section exposes >=9 double channels"), Channels.Num() >= 9))
    {
        return true;
    }
    // Location channels each got exactly one key; rotation/scale untouched.
    TestEqual(TEXT("location.X channel has 1 key"), Channels[0]->GetData().GetTimes().Num(), 1);
    TestEqual(TEXT("location.Y channel has 1 key"), Channels[1]->GetData().GetTimes().Num(), 1);
    TestEqual(TEXT("location.Z channel has 1 key"), Channels[2]->GetData().GetTimes().Num(), 1);
    TestEqual(TEXT("rotation.Roll channel untouched"), Channels[3]->GetData().GetTimes().Num(), 0);
    TestEqual(TEXT("scale.X channel untouched"), Channels[6]->GetData().GetTimes().Num(), 0);

    // Key COUNTS alone cannot see which axis went where, nor whether the value survived:
    // the three requested components are distinct and asymmetric (-800 / 400 / 200), so a
    // swapped x<->z mapping or a zeroed value fails here and only here. The handler maps
    // value.{x,y,z} onto channels ChannelBase+{0,1,2} (SequenceHandler.cpp, Location branch).
    if (Channels[0]->GetData().GetValues().Num() == 1
        && Channels[1]->GetData().GetValues().Num() == 1
        && Channels[2]->GetData().GetValues().Num() == 1)
    {
        TestEqual(TEXT("location.X key carries value.x (-800)"),
            Channels[0]->GetData().GetValues()[0].Value, -800.0);
        TestEqual(TEXT("location.Y key carries value.y (400)"),
            Channels[1]->GetData().GetValues()[0].Value, 400.0);
        TestEqual(TEXT("location.Z key carries value.z (200)"),
            Channels[2]->GetData().GetValues()[0].Value, 200.0);
    }
    else
    {
        AddError(TEXT("location channels did not each carry exactly one key value"));
    }

    return true;
}

// ============================================================================
// sequencer.add_section  (all-optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddSectionValidParamsNoCrashTest,
    "PinWright.sequencer.add_section.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddSectionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("startFrame"), 0.0);
    Payload->SetNumberField(TEXT("endFrame"), 100.0);
    TestTrue(TEXT("sequencer.add_section handler found"), InvokeHandler(TEXT("sequencer.add_section"), Payload));
    return true;
}

// ============================================================================
// sequencer.set_tick_resolution  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetTickResolutionValidParamsNoCrashTest,
    "PinWright.sequencer.set_tick_resolution.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetTickResolutionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("resolution"), TEXT("24000"));
    TestTrue(TEXT("sequencer.set_tick_resolution handler found"),
        InvokeHandler(TEXT("sequencer.set_tick_resolution"), Payload));
    return true;
}

// ============================================================================
// sequencer.set_view_range  (all-optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetViewRangeValidParamsNoCrashTest,
    "PinWright.sequencer.set_view_range.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetViewRangeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("start"), 0.0);
    Payload->SetNumberField(TEXT("end"), 5.0);
    TestTrue(TEXT("sequencer.set_view_range handler found"),
        InvokeHandler(TEXT("sequencer.set_view_range"), Payload));
    return true;
}

// ============================================================================
// sequencer.set_track_muted  (all-optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetTrackMutedValidParamsNoCrashTest,
    "PinWright.sequencer.set_track_muted.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetTrackMutedValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("trackName"), TEXT("AudioTrack"));
    Payload->SetBoolField(TEXT("muted"), true);
    TestTrue(TEXT("sequencer.set_track_muted handler found"),
        InvokeHandler(TEXT("sequencer.set_track_muted"), Payload));
    return true;
}

// ============================================================================
// sequencer.set_track_solo  (all-optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetTrackSoloValidParamsNoCrashTest,
    "PinWright.sequencer.set_track_solo.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetTrackSoloValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("trackName"), TEXT("AudioTrack"));
    Payload->SetBoolField(TEXT("solo"), true);
    TestTrue(TEXT("sequencer.set_track_solo handler found"),
        InvokeHandler(TEXT("sequencer.set_track_solo"), Payload));
    return true;
}

// ============================================================================
// sequencer.set_track_locked  (all-optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetTrackLockedValidParamsNoCrashTest,
    "PinWright.sequencer.set_track_locked.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetTrackLockedValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("trackName"), TEXT("AudioTrack"));
    Payload->SetBoolField(TEXT("locked"), true);
    TestTrue(TEXT("sequencer.set_track_locked handler found"),
        InvokeHandler(TEXT("sequencer.set_track_locked"), Payload));
    return true;
}

// ============================================================================
// sequencer.remove_track  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRemoveTrackValidParamsNoCrashTest,
    "PinWright.sequencer.remove_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceRemoveTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("trackName"), TEXT("AudioTrack"));
    TestTrue(TEXT("sequencer.remove_track handler found"), InvokeHandler(TEXT("sequencer.remove_track"), Payload));
    return true;
}

// ============================================================================
// sequencer.list_track_types  (RPC_NO_PARAMS)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceListTrackTypesValidParamsNoCrashTest,
    "PinWright.sequencer.list_track_types.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceListTrackTypesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("sequencer.list_track_types handler found"),
        InvokeHandler(TEXT("sequencer.list_track_types"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_track
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddTrackValidParamsNoCrashTest,
    "PinWright.sequencer.add_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("trackType"), TEXT("Audio"));
    TestTrue(TEXT("sequencer.add_track handler found"), InvokeHandler(TEXT("sequencer.add_track"), Payload));
    return true;
}

// ============================================================================
// sequencer.list_tracks  (all-optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceListTracksValidParamsNoCrashTest,
    "PinWright.sequencer.list_tracks.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceListTracksValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Sequences/MySeq"));
    TestTrue(TEXT("sequencer.list_tracks handler found"), InvokeHandler(TEXT("sequencer.list_tracks"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceListSectionsReturnsDumpParityFieldsTest,
    "PinWright.sequencer.list_sections.ReturnsDumpParityFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceListSectionsReturnsDumpParityFieldsTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/SequenceListSections_%s"), *Suffix);
    const FString AssetName = FString::Printf(TEXT("SequenceListSections_%s"), *Suffix);
    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    ULevelSequence* Sequence = NewObject<ULevelSequence>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("LevelSequence created"), Sequence))
    {
        return true;
    }
    Sequence->Initialize();

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene created"), MovieScene))
    {
        return true;
    }

    UMovieSceneTrack* Track = MovieScene->AddTrack(UMovieSceneFloatTrack::StaticClass());
    if (!TestNotNull(TEXT("Float track added"), Track))
    {
        return true;
    }

    UMovieSceneSection* Section = Track->CreateNewSection();
    if (!TestNotNull(TEXT("Section created"), Section))
    {
        return true;
    }
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(100)));
    Track->AddSection(*Section);

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.list_sections handler found"),
        InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), Payload, Capture));
    TestTrue(TEXT("sequencer.list_sections succeeded"), Capture.bSuccess);
    TestTrue(TEXT("sequencer.list_sections returned result"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    double SectionCount = 0.0;
    TestTrue(TEXT("sectionCount field present"),
        Capture.Result->TryGetNumberField(TEXT("sectionCount"), SectionCount));
    TestEqual(TEXT("sectionCount is 1"), SectionCount, 1.0);
    FString SequencePath;
    TestTrue(TEXT("sequencePath field present"),
        Capture.Result->TryGetStringField(TEXT("sequencePath"), SequencePath));
    TestEqual(TEXT("sequencePath echoes request"), SequencePath, ObjectPath);

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    if (!TestTrue(TEXT("sections array field present"),
        Capture.Result->TryGetArrayField(TEXT("sections"), Sections)))
    {
        return true;
    }
    if (!TestEqual(TEXT("sections contains one item"), Sections->Num(), 1))
    {
        return true;
    }

    TSharedPtr<FJsonObject> SectionObj = (*Sections)[0]->AsObject();
    if (!TestTrue(TEXT("first section is object"), SectionObj.IsValid()))
    {
        return true;
    }

    FString TrackName;
    TestTrue(TEXT("trackName field present"), SectionObj->TryGetStringField(TEXT("trackName"), TrackName));
    TestFalse(TEXT("trackName is not empty"), TrackName.IsEmpty());
    FString TrackClass;
    TestTrue(TEXT("trackClass field present"), SectionObj->TryGetStringField(TEXT("trackClass"), TrackClass));
    TestFalse(TEXT("trackClass is not empty"), TrackClass.IsEmpty());
    FString BindingGuid;
    TestTrue(TEXT("bindingGuid field present"), SectionObj->TryGetStringField(TEXT("bindingGuid"), BindingGuid));
    TestEqual(TEXT("master section bindingGuid is empty"), BindingGuid, FString());

    double RowIndex = -1.0;
    TestTrue(TEXT("rowIndex field present"), SectionObj->TryGetNumberField(TEXT("rowIndex"), RowIndex));
    TestEqual(TEXT("rowIndex is default row"), RowIndex, 0.0);
    TestTrue(TEXT("blendType field exists"), SectionObj->HasField(TEXT("blendType")));

    const TSharedPtr<FJsonObject>* RangeObj = nullptr;
    if (TestTrue(TEXT("range object field present"), SectionObj->TryGetObjectField(TEXT("range"), RangeObj)))
    {
        TestEqual(TEXT("range.start is 0"), (*RangeObj)->GetNumberField(TEXT("start")), 0.0);
        TestEqual(TEXT("range.end is 100"), (*RangeObj)->GetNumberField(TEXT("end")), 100.0);
    }

    const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
    if (TestTrue(TEXT("channels array field present"), SectionObj->TryGetArrayField(TEXT("channels"), Channels)))
    {
        for (const TSharedPtr<FJsonValue>& ChannelValue : *Channels)
        {
            TSharedPtr<FJsonObject> ChannelObj = ChannelValue.IsValid() ? ChannelValue->AsObject() : nullptr;
            if (TestTrue(TEXT("channel entry is object"), ChannelObj.IsValid()))
            {
                TestTrue(TEXT("channel type field present"), ChannelObj->HasTypedField<EJson::String>(TEXT("type")));
                TestTrue(TEXT("channel keyCount field present"), ChannelObj->HasTypedField<EJson::Number>(TEXT("keyCount")));
                // includeKeys defaults off — the compact payload must NOT carry a keys[] array.
                TestFalse(TEXT("channel keys field absent by default"), ChannelObj->HasField(TEXT("keys")));
            }
        }
    }

    return true;
}

// Regression test for F-sequencer-list-channels-keyframe-readback: with includeKeys=true,
// sequencer.list_sections must surface per-key {frame, value} on each channel so an authored
// keyframe round-trip is verifiable. Before the fix BuildChannelEntriesJson emitted only
// {type, keyCount}; this test asserts the exact frames and values come back and would fail if
// the keys[] enrichment were reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceListSectionsIncludeKeysReturnsPerKeyDataTest,
    "PinWright.sequencer.list_sections.IncludeKeysReturnsPerKeyData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceListSectionsIncludeKeysReturnsPerKeyDataTest::RunTest(const FString& Parameters)
{
    FScopedRegisteredSequence Fixture(TEXT("SeqListSectionsIncludeKeys"));
    if (!TestTrue(TEXT("registered sequence fixture valid"), Fixture.IsValid()))
    {
        return true;
    }

    UMovieScene* MovieScene = Fixture.Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene available"), MovieScene))
    {
        return true;
    }

    UMovieSceneTrack* Track = MovieScene->AddTrack(UMovieSceneFloatTrack::StaticClass());
    if (!TestNotNull(TEXT("Float track added"), Track))
    {
        return true;
    }
    UMovieSceneSection* Section = Track->CreateNewSection();
    if (!TestNotNull(TEXT("Section created"), Section))
    {
        return true;
    }
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(200)));
    Track->AddSection(*Section);

    // Author two keys at known frames/values directly on the production float channel.
    TArrayView<FMovieSceneFloatChannel*> FloatChannels =
        Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
    if (!TestTrue(TEXT("section exposes a float channel"), FloatChannels.Num() >= 1))
    {
        return true;
    }
    FloatChannels[0]->AddLinearKey(FFrameNumber(0), 0.0f);
    FloatChannels[0]->AddLinearKey(FFrameNumber(150), 300.0f);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), Fixture.ObjectPath);
    Payload->SetBoolField(TEXT("includeKeys"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.list_sections handler found"),
        InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), Payload, Capture));
    if (!TestTrue(TEXT("sequencer.list_sections succeeded"), Capture.bSuccess) || !Capture.Result.IsValid())
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    if (!TestTrue(TEXT("sections array present"), Capture.Result->TryGetArrayField(TEXT("sections"), Sections))
        || !TestTrue(TEXT("at least one section"), Sections->Num() >= 1))
    {
        return true;
    }

    // Find the float channel (keyCount==2) and verify its per-key frame/value payload.
    bool bFoundKeyedChannel = false;
    for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
    {
        TSharedPtr<FJsonObject> SectionObj = SectionValue.IsValid() ? SectionValue->AsObject() : nullptr;
        const TArray<TSharedPtr<FJsonValue>>* ChannelsArr = nullptr;
        if (!SectionObj.IsValid() || !SectionObj->TryGetArrayField(TEXT("channels"), ChannelsArr))
        {
            continue;
        }
        for (const TSharedPtr<FJsonValue>& ChannelValue : *ChannelsArr)
        {
            TSharedPtr<FJsonObject> ChannelObj = ChannelValue.IsValid() ? ChannelValue->AsObject() : nullptr;
            if (!ChannelObj.IsValid())
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!ChannelObj->TryGetArrayField(TEXT("keys"), Keys) || Keys->Num() != 2)
            {
                continue;
            }
            bFoundKeyedChannel = true;

            TSharedPtr<FJsonObject> Key0 = (*Keys)[0]->AsObject();
            TSharedPtr<FJsonObject> Key1 = (*Keys)[1]->AsObject();
            if (!TestTrue(TEXT("key entries are objects"), Key0.IsValid() && Key1.IsValid()))
            {
                return true;
            }
            TestEqual(TEXT("key 0 frame is 0"), Key0->GetNumberField(TEXT("frame")), 0.0);
            TestEqual(TEXT("key 1 frame is 150"), Key1->GetNumberField(TEXT("frame")), 150.0);
            double Value0 = -1.0;
            double Value1 = -1.0;
            TestTrue(TEXT("key 0 has value"), Key0->TryGetNumberField(TEXT("value"), Value0));
            TestTrue(TEXT("key 1 has value"), Key1->TryGetNumberField(TEXT("value"), Value1));
            TestEqual(TEXT("key 0 value is 0"), Value0, 0.0);
            TestEqual(TEXT("key 1 value is 300"), Value1, 300.0);
        }
    }
    TestTrue(TEXT("found a channel carrying per-key frame/value data"), bFoundKeyedChannel);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerGetCameraCutTrackReturnsDumpShapeTest,
    "PinWright.sequencer.get_camera_cut_track.ReturnsDumpShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerGetCameraCutTrackReturnsDumpShapeTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/SequenceCameraCut_%s"), *Suffix);
    const FString AssetName = FString::Printf(TEXT("SequenceCameraCut_%s"), *Suffix);
    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

    ULevelSequence* Sequence = NewObject<ULevelSequence>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("LevelSequence created"), Sequence))
    {
        return true;
    }
    Sequence->Initialize();

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene created"), MovieScene))
    {
        return true;
    }

    UMovieSceneTrack* CameraCutTrack = MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
    if (!TestNotNull(TEXT("Camera-cut track added"), CameraCutTrack))
    {
        return true;
    }

    UMovieSceneSection* Section = CameraCutTrack->CreateNewSection();
    if (!TestNotNull(TEXT("Camera-cut section created"), Section))
    {
        return true;
    }
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(100)));
    CameraCutTrack->AddSection(*Section);

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.get_camera_cut_track handler found"),
        InvokeHandlerWithCapture(TEXT("sequencer.get_camera_cut_track"), Payload, Capture));
    TestTrue(TEXT("sequencer.get_camera_cut_track succeeded"), Capture.bSuccess);
    TestTrue(TEXT("sequencer.get_camera_cut_track returned result"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    FString SequencePath;
    TestTrue(TEXT("sequencePath field present"),
        Capture.Result->TryGetStringField(TEXT("sequencePath"), SequencePath));
    TestEqual(TEXT("sequencePath echoes request"), SequencePath, ObjectPath);

    const TSharedPtr<FJsonObject>* CameraCutTrackObj = nullptr;
    if (!TestTrue(TEXT("cameraCutTrack object exists"),
        Capture.Result->TryGetObjectField(TEXT("cameraCutTrack"), CameraCutTrackObj)))
    {
        return true;
    }

    FString TrackClass;
    TestTrue(TEXT("cameraCutTrack.class field present"), (*CameraCutTrackObj)->TryGetStringField(TEXT("class"), TrackClass));
    TestFalse(TEXT("cameraCutTrack.class is not empty"), TrackClass.IsEmpty());
    TestEqual(TEXT("cameraCutTrack.class is MovieSceneCameraCutTrack"), TrackClass, FString(TEXT("MovieSceneCameraCutTrack")));

    double SectionCount = 0.0;
    TestTrue(TEXT("cameraCutTrack.sectionCount field present"),
        (*CameraCutTrackObj)->TryGetNumberField(TEXT("sectionCount"), SectionCount));
    TestEqual(TEXT("cameraCutTrack.sectionCount is 1"), SectionCount, 1.0);

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    if (!TestTrue(TEXT("cameraCutTrack.sections array field present"),
        (*CameraCutTrackObj)->TryGetArrayField(TEXT("sections"), Sections)))
    {
        return true;
    }
    if (!TestEqual(TEXT("cameraCutTrack.sections contains one item"), Sections->Num(), 1))
    {
        return true;
    }

    TSharedPtr<FJsonObject> SectionObj = (*Sections)[0]->AsObject();
    if (!TestTrue(TEXT("first camera-cut section is object"), SectionObj.IsValid()))
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* RangeObj = nullptr;
    if (TestTrue(TEXT("camera-cut section range object field present"), SectionObj->TryGetObjectField(TEXT("range"), RangeObj)))
    {
        TestEqual(TEXT("camera-cut range.start is 0"), (*RangeObj)->GetNumberField(TEXT("start")), 0.0);
        TestEqual(TEXT("camera-cut range.end is 100"), (*RangeObj)->GetNumberField(TEXT("end")), 100.0);
    }

    double RowIndex = -1.0;
    TestTrue(TEXT("camera-cut section rowIndex field present"), SectionObj->TryGetNumberField(TEXT("rowIndex"), RowIndex));
    TestTrue(TEXT("camera-cut section channels field present"), SectionObj->HasField(TEXT("channels")));

    return true;
}

// ============================================================================
// sequencer.set_work_range  (all-optional/defaulted)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceSetWorkRangeValidParamsNoCrashTest,
    "PinWright.sequencer.set_work_range.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceSetWorkRangeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("start"), 0.0);
    Payload->SetNumberField(TEXT("end"), 10.0);
    TestTrue(TEXT("sequencer.set_work_range handler found"),
        InvokeHandler(TEXT("sequencer.set_work_range"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_keyframe  (REQ: sequencePath, bindingGuid, propertyName, time, value)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddKeyframeValidParamsNoCrashTest,
    "PinWright.sequencer.add_keyframe.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddKeyframeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Sequences/MySeq"));
    Payload->SetStringField(TEXT("bindingGuid"), TEXT("00000000-0000-0000-0000-000000000001"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Location"));
    Payload->SetNumberField(TEXT("time"), 1.0);
    Payload->SetNumberField(TEXT("value"), 100.0);
    TestTrue(TEXT("sequencer.add_keyframe handler found"),
        InvokeHandler(TEXT("sequencer.add_keyframe"), Payload));
    return true;
}

// ============================================================================
// sequencer.manage_track  (REQ: sequencePath, bindingGuid, propertyName, op)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerManageTrackValidParamsNoCrashTest,
    "PinWright.sequencer.manage_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerManageTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Sequences/MySeq"));
    Payload->SetStringField(TEXT("bindingGuid"), TEXT("00000000-0000-0000-0000-000000000001"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Location"));
    Payload->SetStringField(TEXT("op"), TEXT("add"));
    TestTrue(TEXT("sequencer.manage_track handler found"),
        InvokeHandler(TEXT("sequencer.manage_track"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_camera_track  (REQ: sequencePath, cameraActorPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddCameraTrackValidParamsNoCrashTest,
    "PinWright.sequencer.add_camera_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddCameraTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Sequences/MySeq"));
    Payload->SetStringField(TEXT("cameraActorPath"), TEXT("/Game/Maps/MyLevel.MyLevel:PersistentLevel.CameraActor_0"));
    Payload->SetNumberField(TEXT("startTime"), 0.0);
    Payload->SetNumberField(TEXT("endTime"), 5.0);
    TestTrue(TEXT("sequencer.add_camera_track handler found"),
        InvokeHandler(TEXT("sequencer.add_camera_track"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_animation_track  (REQ: sequencePath, bindingGuid, animSequencePath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddAnimationTrackValidParamsNoCrashTest,
    "PinWright.sequencer.add_animation_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddAnimationTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Sequences/MySeq"));
    Payload->SetStringField(TEXT("bindingGuid"), TEXT("00000000-0000-0000-0000-000000000001"));
    Payload->SetStringField(TEXT("animSequencePath"), TEXT("/Game/Animations/Run"));
    Payload->SetNumberField(TEXT("startTime"), 0.0);
    TestTrue(TEXT("sequencer.add_animation_track handler found"),
        InvokeHandler(TEXT("sequencer.add_animation_track"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_transform_track  (REQ: sequencePath, bindingGuid)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddTransformTrackValidParamsNoCrashTest,
    "PinWright.sequencer.add_transform_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddTransformTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Sequences/MySeq"));
    Payload->SetStringField(TEXT("bindingGuid"), TEXT("00000000-0000-0000-0000-000000000001"));
    TestTrue(TEXT("sequencer.add_transform_track handler found"),
        InvokeHandler(TEXT("sequencer.add_transform_track"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_camera_rig_rail  (REQ: sequencePath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddCameraRigRailValidParamsNoCrashTest,
    "PinWright.sequencer.add_camera_rig_rail.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddCameraRigRailValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Test/NonExistent"));
    TestTrue(TEXT("sequencer.add_camera_rig_rail handler found"),
        InvokeHandler(TEXT("sequencer.add_camera_rig_rail"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_camera_rig_crane  (REQ: sequencePath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddCameraRigCraneValidParamsNoCrashTest,
    "PinWright.sequencer.add_camera_rig_crane.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddCameraRigCraneValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Test/NonExistent"));
    TestTrue(TEXT("sequencer.add_camera_rig_crane handler found"),
        InvokeHandler(TEXT("sequencer.add_camera_rig_crane"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_audio_track  (REQ: sequencePath, soundPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddAudioTrackValidParamsNoCrashTest,
    "PinWright.sequencer.add_audio_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddAudioTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), TEXT("/Game/Test/NonExistent"));
    Payload->SetStringField(TEXT("soundPath"), TEXT("/Game/Test/NonExistentSound"));
    Payload->SetNumberField(TEXT("volume"), 0.5);
    Payload->SetNumberField(TEXT("pitch"), 1.2);
    TestTrue(TEXT("sequencer.add_audio_track handler found"),
        InvokeHandler(TEXT("sequencer.add_audio_track"), Payload));
    return true;
}

// ============================================================================
// sequencer.add_sub_sequence  (REQ: path, innerSequencePath, startFrame, durationFrames)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddSubSequenceValidParamsNoCrashTest,
    "PinWright.sequencer.add_sub_sequence.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddSubSequenceValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Test/NonExistent"));
    Payload->SetStringField(TEXT("innerSequencePath"), TEXT("/Game/Test/NonExistentInner"));
    Payload->SetNumberField(TEXT("startFrame"), 0);
    Payload->SetNumberField(TEXT("durationFrames"), 240);
    Payload->SetNumberField(TEXT("timeScale"), 0.5);
    TestTrue(TEXT("sequencer.add_sub_sequence handler found"),
        InvokeHandler(TEXT("sequencer.add_sub_sequence"), Payload));
    return true;
}
