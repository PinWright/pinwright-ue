// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the camera.* capture handlers (camera.frame_actor / camera.orbit_shots).
//
// Both verbs drive the LIVE Level Editor viewport through CaptureEditorViewportToPng,
// which is only reachable when a real active viewport exists. Under headless / -unattended
// automation there may be no active level viewport, so every capture test follows the same
// RHI-guard pattern as TestRenderHandlers.cpp: a success asserts the PNG path exists on disk;
// a failure must be TYPED (NO_ACTIVE_LEVEL_VIEWPORT / CAPTURE_FAILED / ...) and the pixel/shape
// assertions are skipped. Argument-validation and unknown-param tests need no viewport and run
// deterministically.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Handlers/Render/CameraShotPlanUtils.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"

namespace
{
    int32 PWCamCountWorldActors()
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return -1;
        }
        int32 Count = 0;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            ++Count;
        }
        return Count;
    }

    // The live Level Editor viewport client, or nullptr when none is active (headless).
    FEditorViewportClient* PWCamLiveViewportClient()
    {
        FLevelEditorModule* Module = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (!Module)
        {
            return nullptr;
        }
        TSharedPtr<IAssetViewport> ActiveViewport = Module->GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            return nullptr;
        }
        return &ActiveViewport->GetAssetViewportClient();
    }

    // True when this failure is one of the typed, non-crashing exits the camera handlers
    // are allowed to return when the live capture cannot complete in this run.
    // NO_ACTIVE_LEVEL_VIEWPORT / NO_EDITOR_WORLD / EDITOR_NOT_AVAILABLE come from the
    // viewport-acquisition step; CAPTURE_FAILED / PREVIEW_VIEWPORT_NOT_FOUND / ENCODE_FAILED
    // / SAVE_FAILED come from CaptureEditorViewportToPng. All are typed, known codes — an
    // empty/unknown code still fails the assertion.
    //
    // ACTOR_NOT_FOUND is deliberately NOT in this list. It used to be, described as a
    // legitimate skip because the fixture spawned RF_Transient and the handlers resolve
    // through FindActorByName -> UEditorActorSubsystem::GetAllLevelActors, which drops
    // transient actors (UE 5.8 EditorActorSubsystem.cpp:386 `!Actor->HasAnyFlags(RF_Transient)`).
    // That is not an environment-dependent skip: it fired on EVERY run, interactive included,
    // so every live camera test below exited before the verb did any work and their whole
    // success branch was dead code. The fixture now spawns non-transient (see
    // PWCamSpawnResolvableCubeActor), so an unresolved target is a real defect and must fail.
    bool PWCamIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED");
    }

    // Spawns a NON-transient AStaticMeshActor carrying the engine unit cube at Location with
    // the given editor label. Same rationale (and same shape) as
    // AnnotatedSpawnResolvableCubeActor in TestAnnotatedCaptureHandlers.cpp: camera.frame_actor
    // and camera.orbit_shots resolve their target through McpActorUtils::FindActorByName(nullptr,
    // ...) -> UEditorActorSubsystem::GetAllLevelActors, which excludes RF_Transient actors in
    // non-play worlds. The shared SpawnTransientCubeActor fixture is therefore permanently
    // unresolvable to these verbs. Returns nullptr when there is no world, the engine cube is
    // unavailable, or the spawn fails. Pair with FScopedEditorWorldActorGuard, which destroys
    // whatever the test spawned and restores the level's dirty flag on scope exit.
    AStaticMeshActor* PWCamSpawnResolvableCubeActor(UWorld* World, const FString& Label,
        const FVector& Location)
    {
        if (!World)
        {
            return nullptr;
        }
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!CubeMesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams; // deliberately NOT RF_Transient (see above)
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    void PWCamDeleteFile(const FString& Path)
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }
}

// ============================================================================
// camera.frame_actor — argument validation (no viewport required)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFrameActorMissingActorTest,
    "PinWright.camera.frame_actor.MissingActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFrameActorMissingActorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.frame_actor handler found"),
        InvokeHandlerWithCapture(TEXT("camera.frame_actor"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("missing actor target fails"), Capture.bSuccess);
    TestEqual(TEXT("missing actor error is typed"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFrameActorUnknownActorTest,
    "PinWright.camera.frame_actor.UnknownActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFrameActorUnknownActorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"),
        FString::Printf(TEXT("PW_NoSuchActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.frame_actor handler found"),
        InvokeHandlerWithCapture(TEXT("camera.frame_actor"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("unknown actor fails"), Capture.bSuccess);
    TestEqual(TEXT("unknown actor error is ACTOR_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

// Unknown params are rejected by the dispatcher (before the handler body), so this
// routes through the real dispatcher rather than the direct InvokeHandler path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFrameActorUnknownParamRejectedTest,
    "PinWright.camera.frame_actor.UnknownParamRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFrameActorUnknownParamRejectedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TEXT("AnyActor"));
    Params->SetStringField(TEXT("bogusUnknownArg"), TEXT("x"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("camera.frame_actor"),
        TEXT("req-cam-frame-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param rejected"), bSuccess);
    TestEqual(TEXT("unknown param error is UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// ============================================================================
// camera.frame_actor — live capture against a spawned actor (RHI-guarded)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFrameActorCapturesSpawnedActorTest,
    "PinWright.camera.frame_actor.CapturesSpawnedActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFrameActorCapturesSpawnedActorTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamFrame_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSpawnResolvableCubeActor(World, Label, FVector(0.0f, 0.0f, 0.0f));
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }
    // Resolve by the label the handler will actually see (SetActorLabel may disambiguate).
    const FString ResolvedName = Actor->GetActorLabel();

    // Snapshot the live viewport camera so we can assert it is restored after the capture.
    FEditorViewportClient* Client = PWCamLiveViewportClient();
    FVector CamLocBefore = FVector::ZeroVector;
    FRotator CamRotBefore = FRotator::ZeroRotator;
    ELevelViewportType CamTypeBefore = LVT_Perspective;
    if (Client)
    {
        CamLocBefore = Client->GetViewLocation();
        CamRotBefore = Client->GetViewRotation();
        CamTypeBefore = Client->GetViewportType();
    }

    const int32 ActorCountBefore = PWCamCountWorldActors();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ResolvedName);
    Payload->SetNumberField(TEXT("azimuth"), 45.0);
    Payload->SetNumberField(TEXT("elevation"), 30.0);
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.frame_actor handler found"),
        InvokeHandlerWithCapture(TEXT("camera.frame_actor"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    FString CapturedPath;
    if (Capture.bSuccess)
    {
        TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
        if (Capture.Result.IsValid())
        {
            TestTrue(TEXT("success carries a path"), Capture.Result->TryGetStringField(TEXT("path"), CapturedPath));
            TestTrue(TEXT("captured PNG exists on disk"),
                !CapturedPath.IsEmpty() && IFileManager::Get().FileExists(*CapturedPath));
            TestEqual(TEXT("capture width echoed"),
                static_cast<int32>(Capture.Result->GetNumberField(TEXT("width"))), 256);
            TestEqual(TEXT("capture height echoed"),
                static_cast<int32>(Capture.Result->GetNumberField(TEXT("height"))), 256);
            FString EchoedActor;
            TestTrue(TEXT("response echoes actorName"),
                Capture.Result->TryGetStringField(TEXT("actorName"), EchoedActor));
            TestEqual(TEXT("echoed actorName matches"), EchoedActor, ResolvedName);
        }
    }
    else
    {
        TestTrue(TEXT("capture failure is typed"), PWCamIsTypedCaptureFailure(Capture.ErrorCode));
        AddInfo(TEXT("Skipped pixel checks: live viewport capture unavailable in this run."));
    }
    PWCamDeleteFile(CapturedPath);

    // The capture must not spawn or destroy level actors.
    const int32 ActorCountAfter = PWCamCountWorldActors();
    if (ActorCountBefore >= 0 && ActorCountAfter >= 0)
    {
        TestEqual(TEXT("frame_actor does not change the level actor count"), ActorCountAfter, ActorCountBefore);
    }

    // The handler moves the real editor camera then must restore it (scope-exit in the util).
    if (Client)
    {
        TestTrue(TEXT("viewport camera location restored"),
            Client->GetViewLocation().Equals(CamLocBefore, 0.5));
        TestTrue(TEXT("viewport camera rotation restored"),
            Client->GetViewRotation().Equals(CamRotBefore, 0.5));
        TestEqual(TEXT("viewport type restored"),
            static_cast<int32>(Client->GetViewportType()), static_cast<int32>(CamTypeBefore));
    }
    return true;
}

// ============================================================================
// camera.orbit_shots — argument validation (no viewport required)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsMissingTargetTest,
    "PinWright.camera.orbit_shots.MissingTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsMissingTargetTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("missing target fails"), Capture.bSuccess);
    TestEqual(TEXT("missing target error is typed"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsPointRequiresRadiusTest,
    "PinWright.camera.orbit_shots.PointRequiresRadius",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsPointRequiresRadiusTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
    Point->SetNumberField(TEXT("x"), 0.0);
    Point->SetNumberField(TEXT("y"), 0.0);
    Point->SetNumberField(TEXT("z"), 0.0);
    Payload->SetObjectField(TEXT("point"), Point);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("bare point without radius fails"), Capture.bSuccess);
    TestEqual(TEXT("bare point error is typed"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsUnknownParamRejectedTest,
    "PinWright.camera.orbit_shots.UnknownParamRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsUnknownParamRejectedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TEXT("AnyActor"));
    Params->SetStringField(TEXT("bogusUnknownArg"), TEXT("x"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("camera.orbit_shots"),
        TEXT("req-cam-orbit-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param rejected"), bSuccess);
    TestEqual(TEXT("unknown param error is UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// ============================================================================
// camera.orbit_shots — live capture against a spawned actor (RHI-guarded)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsDefaultSetTest,
    "PinWright.camera.orbit_shots.DefaultSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsDefaultSetTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamOrbit_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSpawnResolvableCubeActor(World, Label, FVector(0.0f, 0.0f, 0.0f));
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }
    const FString ResolvedName = Actor->GetActorLabel();

    FEditorViewportClient* Client = PWCamLiveViewportClient();
    FVector CamLocBefore = FVector::ZeroVector;
    FRotator CamRotBefore = FRotator::ZeroRotator;
    ELevelViewportType CamTypeBefore = LVT_Perspective;
    if (Client)
    {
        CamLocBefore = Client->GetViewLocation();
        CamRotBefore = Client->GetViewRotation();
        CamTypeBefore = Client->GetViewportType();
    }

    const int32 ActorCountBefore = PWCamCountWorldActors();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ResolvedName);
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    TArray<FString> CapturedPaths;
    if (Capture.bSuccess)
    {
        TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
        if (Capture.Result.IsValid())
        {
            // The default (no count/angles) canonical set is exactly 4 shots.
            TestEqual(TEXT("count field is 4"),
                static_cast<int32>(Capture.Result->GetNumberField(TEXT("count"))), 4);
            const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
            TestTrue(TEXT("shots array present"), Capture.Result->TryGetArrayField(TEXT("shots"), Shots));
            if (Shots)
            {
                TestEqual(TEXT("shots array has 4 entries"), Shots->Num(), 4);
                for (const TSharedPtr<FJsonValue>& ShotVal : *Shots)
                {
                    const TSharedPtr<FJsonObject>* ShotObj = nullptr;
                    if (ShotVal.IsValid() && ShotVal->TryGetObject(ShotObj) && ShotObj && (*ShotObj).IsValid())
                    {
                        FString ShotPath;
                        TestTrue(TEXT("shot carries a path"), (*ShotObj)->TryGetStringField(TEXT("path"), ShotPath));
                        if (!ShotPath.IsEmpty())
                        {
                            CapturedPaths.Add(ShotPath);
                            TestTrue(TEXT("shot PNG exists on disk"), IFileManager::Get().FileExists(*ShotPath));
                        }
                    }
                }
            }
        }
    }
    else
    {
        TestTrue(TEXT("capture failure is typed"), PWCamIsTypedCaptureFailure(Capture.ErrorCode));
        AddInfo(TEXT("Skipped shot checks: live viewport capture unavailable in this run."));
    }
    for (const FString& Path : CapturedPaths)
    {
        PWCamDeleteFile(Path);
    }

    const int32 ActorCountAfter = PWCamCountWorldActors();
    if (ActorCountBefore >= 0 && ActorCountAfter >= 0)
    {
        TestEqual(TEXT("orbit_shots does not change the level actor count"), ActorCountAfter, ActorCountBefore);
    }

    if (Client)
    {
        TestTrue(TEXT("viewport camera location restored"),
            Client->GetViewLocation().Equals(CamLocBefore, 0.5));
        TestTrue(TEXT("viewport camera rotation restored"),
            Client->GetViewRotation().Equals(CamRotBefore, 0.5));
        TestEqual(TEXT("viewport type restored"),
            static_cast<int32>(Client->GetViewportType()), static_cast<int32>(CamTypeBefore));
    }
    return true;
}

// Explicit count produces a uniform perspective ring of exactly that many shots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsExplicitCountTest,
    "PinWright.camera.orbit_shots.ExplicitCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsExplicitCountTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamOrbitN_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSpawnResolvableCubeActor(World, Label, FVector(0.0f, 0.0f, 0.0f));
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }
    const FString ResolvedName = Actor->GetActorLabel();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ResolvedName);
    Payload->SetNumberField(TEXT("count"), 3.0);
    Payload->SetNumberField(TEXT("radius"), 500.0);
    Payload->SetNumberField(TEXT("width"), 128.0);
    Payload->SetNumberField(TEXT("height"), 128.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    TArray<FString> CapturedPaths;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestEqual(TEXT("count field is 3"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("count"))), 3);
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots)
        {
            TestEqual(TEXT("shots array has 3 entries"), Shots->Num(), 3);
            for (const TSharedPtr<FJsonValue>& ShotVal : *Shots)
            {
                const TSharedPtr<FJsonObject>* ShotObj = nullptr;
                if (ShotVal.IsValid() && ShotVal->TryGetObject(ShotObj) && ShotObj && (*ShotObj).IsValid())
                {
                    FString ShotPath;
                    if ((*ShotObj)->TryGetStringField(TEXT("path"), ShotPath) && !ShotPath.IsEmpty())
                    {
                        CapturedPaths.Add(ShotPath);
                    }
                }
            }
        }
    }
    else if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"), PWCamIsTypedCaptureFailure(Capture.ErrorCode));
    }
    for (const FString& Path : CapturedPaths)
    {
        PWCamDeleteFile(Path);
    }
    return true;
}

// Regression for the overwrite bug (B-camera-orbit-shots-collide-on-timestamp): every
// shot in an orbit set must report a DISTINCT `path`. Pre-fix, shots left `Request.filename`
// empty, so CaptureEditorViewportToPng auto-named each from a SECOND-resolution timestamp;
// a set captured inside one wall-clock second collided on a single filename and overwrote
// itself on disk, leaving only the last-per-second PNG even though the response still
// listed every shot. The distinctness check is RHI-independent (it inspects the response's
// path strings, computed regardless of whether pixels render), so a headless run that
// succeeds still exercises it; a headless run that fails with a typed capture error before
// producing shots is skipped like the sibling capture tests.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsDistinctShotPathsTest,
    "PinWright.camera.orbit_shots.DistinctShotPaths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsDistinctShotPathsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamOrbitDistinct_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSpawnResolvableCubeActor(World, Label, FVector(0.0f, 0.0f, 0.0f));
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }
    const FString ResolvedName = Actor->GetActorLabel();

    // The default (no count/angles) canonical set is the exact reported repro: 4 shots
    // captured back-to-back, well within one second.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ResolvedName);
    Payload->SetNumberField(TEXT("width"), 128.0);
    Payload->SetNumberField(TEXT("height"), 128.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    TArray<FString> AllPaths;   // every shot's reported path (also used for teardown)
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        TestTrue(TEXT("shots array present"), Capture.Result->TryGetArrayField(TEXT("shots"), Shots));
        if (Shots)
        {
            for (const TSharedPtr<FJsonValue>& ShotVal : *Shots)
            {
                const TSharedPtr<FJsonObject>* ShotObj = nullptr;
                if (ShotVal.IsValid() && ShotVal->TryGetObject(ShotObj) && ShotObj && (*ShotObj).IsValid())
                {
                    FString ShotPath;
                    TestTrue(TEXT("shot carries a path"), (*ShotObj)->TryGetStringField(TEXT("path"), ShotPath));
                    if (!ShotPath.IsEmpty())
                    {
                        AllPaths.Add(ShotPath);
                    }
                }
            }

            // Core regression assertion: the number of distinct paths must equal the
            // number of shots — no two shots share a filename (which would mean one
            // overwrote the other on disk).
            TArray<FString> DistinctPaths;
            for (const FString& Path : AllPaths)
            {
                DistinctPaths.AddUnique(Path);
            }
            TestEqual(TEXT("every shot reports a non-empty path"), AllPaths.Num(), Shots->Num());
            TestEqual(TEXT("all shot paths are distinct (no overwrite collision)"),
                DistinctPaths.Num(), AllPaths.Num());
        }
    }
    else if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"), PWCamIsTypedCaptureFailure(Capture.ErrorCode));
        AddInfo(TEXT("Skipped distinctness check: live viewport capture unavailable in this run."));
    }
    for (const FString& Path : AllPaths)
    {
        PWCamDeleteFile(Path);
    }
    return true;
}

// ============================================================================
// camera.orbit_shots — orthographic count/angles shots and the views:"sides" plan
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsViewsExclusiveTest,
    "PinWright.camera.orbit_shots.ViewsExclusiveWithCountAndAngles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsViewsExclusiveTest::RunTest(const FString& Parameters)
{
    // views and count/angles are two different answers to "which poses". Silently ranking one
    // over the other is how a verb ends up with parameters that contradict each other, so the
    // combination is rejected outright. No viewport needed: validation happens before capture.
    TSharedPtr<FJsonObject> WithCount = MakeShared<FJsonObject>();
    WithCount->SetStringField(TEXT("actorName"), TEXT("AnyActor"));
    WithCount->SetStringField(TEXT("views"), TEXT("sides"));
    WithCount->SetNumberField(TEXT("count"), 4.0);

    FTestResponseCapture CountCapture;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), WithCount, CountCapture);
    TestFalse(TEXT("views + count is rejected"), CountCapture.bSuccess);
    TestEqual(TEXT("views + count error is typed"), CountCapture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));

    TSharedPtr<FJsonObject> WithAngles = MakeShared<FJsonObject>();
    WithAngles->SetStringField(TEXT("actorName"), TEXT("AnyActor"));
    WithAngles->SetStringField(TEXT("views"), TEXT("sides"));
    TArray<TSharedPtr<FJsonValue>> Angles;
    TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();
    One->SetNumberField(TEXT("azimuth"), 10.0);
    Angles.Add(MakeShared<FJsonValueObject>(One));
    WithAngles->SetArrayField(TEXT("angles"), Angles);

    FTestResponseCapture AngleCapture;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), WithAngles, AngleCapture);
    TestFalse(TEXT("views + angles is rejected"), AngleCapture.bSuccess);
    TestEqual(TEXT("views + angles error is typed"), AngleCapture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsInvalidViewsAndProjectionTest,
    "PinWright.camera.orbit_shots.InvalidViewsAndProjectionRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsInvalidViewsAndProjectionTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> BadViews = MakeShared<FJsonObject>();
    BadViews->SetStringField(TEXT("actorName"), TEXT("AnyActor"));
    BadViews->SetStringField(TEXT("views"), TEXT("cube"));

    FTestResponseCapture ViewsCapture;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), BadViews, ViewsCapture);
    TestFalse(TEXT("an unknown views value is rejected"), ViewsCapture.bSuccess);
    TestEqual(TEXT("unknown views error is typed"), ViewsCapture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));

    TSharedPtr<FJsonObject> BadProjection = MakeShared<FJsonObject>();
    BadProjection->SetStringField(TEXT("actorName"), TEXT("AnyActor"));
    BadProjection->SetStringField(TEXT("projectionMode"), TEXT("isometric"));

    FTestResponseCapture ProjectionCapture;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), BadProjection, ProjectionCapture);
    TestFalse(TEXT("an unknown projectionMode is rejected"), ProjectionCapture.bSuccess);
    TestEqual(TEXT("unknown projectionMode error is typed"), ProjectionCapture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsSixSidesTest,
    "PinWright.camera.orbit_shots.ViewsSidesGivesSixDistinctOrthoViews",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsSixSidesTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamSides_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSpawnResolvableCubeActor(World, Label, FVector::ZeroVector);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Payload->SetStringField(TEXT("views"), TEXT("sides"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));

    if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"), PWCamIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            TEXT("Skipped six-sides checks: live viewport capture unavailable in this run."));
        return true;
    }

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("views 'sides' succeeded with no result object"));
        return true;
    }

    TestEqual(TEXT("views 'sides' plans exactly 6 shots"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("count"))), 6);

    // The multi-image budget applies to this new opt-in path only, and the response says so.
    FString ResolutionSource;
    Capture.Result->TryGetStringField(TEXT("resolutionSource"), ResolutionSource);
    TestEqual(TEXT("an unsized sides call takes the multi-image budget"),
        ResolutionSource, FString(TEXT("budget")));

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("shots"), Shots) || !Shots)
    {
        AddError(TEXT("no shots array"));
        return true;
    }

    TArray<FString> AllPaths;
    TArray<FString> OrthoViews;
    for (const TSharedPtr<FJsonValue>& ShotVal : *Shots)
    {
        const TSharedPtr<FJsonObject>* ShotObj = nullptr;
        if (!ShotVal.IsValid() || !ShotVal->TryGetObject(ShotObj) || !ShotObj || !(*ShotObj).IsValid())
        {
            continue;
        }

        FString Projection;
        (*ShotObj)->TryGetStringField(TEXT("projectionMode"), Projection);
        TestEqual(TEXT("every side shot is orthographic by default"), Projection,
            FString(TEXT("orthographic")));

        // The six poses are chosen to land exactly on a cardinal axis, so the snap must be a
        // no-op. A reported snap means the plan's azimuth/elevation spellings drifted (e.g.
        // 270 instead of -90, or -180 instead of 180) and the response would be advertising a
        // pose the caller never asked for.
        bool bSnapped = false;
        (*ShotObj)->TryGetBoolField(TEXT("orthoAxisSnapped"), bSnapped);
        TestFalse(TEXT("a side shot is already axis-aligned and is not snapped"), bSnapped);

        double ShotWidth = 0.0;
        (*ShotObj)->TryGetNumberField(TEXT("width"), ShotWidth);
        TestEqual(TEXT("budgeted side shots take the orbit views budget edge"),
            static_cast<int32>(ShotWidth), PinWrightCameraFrame::GOrbitViewsBudgetEdge);

        FString OrthoView;
        if ((*ShotObj)->TryGetStringField(TEXT("orthoView"), OrthoView))
        {
            OrthoViews.AddUnique(OrthoView);
        }

        FString ShotPath;
        if ((*ShotObj)->TryGetStringField(TEXT("path"), ShotPath) && !ShotPath.IsEmpty())
        {
            AllPaths.AddUnique(ShotPath);
            TestTrue(TEXT("side shot PNG exists on disk"), IFileManager::Get().FileExists(*ShotPath));
        }
    }

    // The whole point of the plan: six DIFFERENT views, not the same axis six times. Before the
    // orthographic plan existed this was six separate camera.frame_actor calls.
    TestEqual(TEXT("the six sides resolve to six distinct orthographic views"), OrthoViews.Num(), 6);
    TestEqual(TEXT("all six side shots land on distinct files"), AllPaths.Num(), Shots->Num());

    for (const FString& Path : AllPaths)
    {
        PWCamDeleteFile(Path);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsOrthographicCountTest,
    "PinWright.camera.orbit_shots.OrthographicAppliesToCountShots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsOrthographicCountTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamOrtho_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSpawnResolvableCubeActor(World, Label, FVector::ZeroVector);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Payload->SetNumberField(TEXT("count"), 2.0);
    Payload->SetNumberField(TEXT("elevation"), 0.0);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture);

    if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"), PWCamIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            TEXT("Skipped orthographic-count checks: live viewport capture unavailable."));
        return true;
    }

    TArray<FString> Paths;
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots)
    {
        TestEqual(TEXT("count 2 plans two shots"), Shots->Num(), 2);
        for (const TSharedPtr<FJsonValue>& ShotVal : *Shots)
        {
            const TSharedPtr<FJsonObject>* ShotObj = nullptr;
            if (!ShotVal.IsValid() || !ShotVal->TryGetObject(ShotObj) || !ShotObj) { continue; }

            // Counterfactual: restoring either hardcoded TEXT("perspective") literal in the
            // count/angles plan builders makes this assertion fail.
            FString Projection;
            (*ShotObj)->TryGetStringField(TEXT("projectionMode"), Projection);
            TestEqual(TEXT("projectionMode reaches count-driven shots"), Projection,
                FString(TEXT("orthographic")));

            // An orthographic shot carries the world-space frame width, never a fov.
            double OrthoWidth = 0.0;
            TestTrue(TEXT("orthographic shot reports orthoWidth"),
                (*ShotObj)->TryGetNumberField(TEXT("orthoWidth"), OrthoWidth));

            FString Path;
            if ((*ShotObj)->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
            {
                Paths.AddUnique(Path);
            }
        }
    }

    // Explicit width/height must keep beating the budget.
    FString ResolutionSource;
    if (Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("resolutionSource"), ResolutionSource);
    }
    TestEqual(TEXT("explicit sizes are reported as caller-chosen"), ResolutionSource,
        FString(TEXT("caller")));

    for (const FString& Path : Paths)
    {
        PWCamDeleteFile(Path);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitShotsLegacyDefaultsUnchangedTest,
    "PinWright.camera.orbit_shots.LegacyDefaultsUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitShotsLegacyDefaultsUnchangedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamLegacy_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSpawnResolvableCubeActor(World, Label, FVector::ZeroVector);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }

    // A pre-existing call shape: count with no size and no projectionMode. It must still be
    // 1024x1024 perspective. The multi-image budget deliberately does NOT reach here, because
    // shrinking it would silently change every archived comparison and rescale
    // world-units-per-pixel for orthographic measurement recipes.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Payload->SetNumberField(TEXT("count"), 1.0);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture);

    if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"), PWCamIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            TEXT("Skipped legacy-default checks: live viewport capture unavailable."));
        return true;
    }

    FString ResolutionSource;
    Capture.Result->TryGetStringField(TEXT("resolutionSource"), ResolutionSource);
    TestEqual(TEXT("a legacy call shape keeps the historical default size"), ResolutionSource,
        FString(TEXT("default")));

    TArray<FString> Paths;
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots && Shots->Num() == 1)
    {
        const TSharedPtr<FJsonObject>* ShotObj = nullptr;
        if ((*Shots)[0]->TryGetObject(ShotObj) && ShotObj)
        {
            double ShotWidth = 0.0;
            (*ShotObj)->TryGetNumberField(TEXT("width"), ShotWidth);
            TestEqual(TEXT("legacy count shots keep the orbit historical default edge"),
                static_cast<int32>(ShotWidth), PinWrightCameraFrame::GOrbitLegacyDefaultEdge);

            FString Projection;
            (*ShotObj)->TryGetStringField(TEXT("projectionMode"), Projection);
            TestEqual(TEXT("legacy count shots are still perspective"), Projection,
                FString(TEXT("perspective")));

            FString Path;
            if ((*ShotObj)->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
            {
                Paths.Add(Path);
            }
        }
    }

    for (const FString& Path : Paths)
    {
        PWCamDeleteFile(Path);
    }
    return true;
}
