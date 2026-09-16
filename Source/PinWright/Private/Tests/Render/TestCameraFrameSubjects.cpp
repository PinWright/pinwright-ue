// Copyright (c) 2026 Alexander Penkin. MIT License.

// What camera.frame_actor and camera.orbit_shots gained when the subject became a parameter:
// the `subject` argument itself, the pose bound surviving the conversion onto the shared
// pose-list primitive, the warm-up frame reaching the single-still verb, and the rule that the
// `subject` RESPONSE block appears only when a subject was actually resolved.
//
// A NEW FILE, not an addition to TestCameraFrameHandlers.cpp. That file is the regression floor
// for these two verbs and has to keep passing unmodified; a chunk that needs a new assertion
// writes its own file so a failure names which contract moved.
//
// SKIPS ARE NAMED, NOT SILENT. Every live-capture assertion below is guarded the same way
// TestCameraFrameHandlers.cpp guards its own: under -unattended there may be no active level
// viewport, so a failure must be TYPED and the pixel/shape assertions are then skipped WITH an
// AddInfo saying which branch was taken. A test that reports success having asserted nothing is
// board ticket B-test-skips-assertions-silently, and the guarded tests here therefore each carry
// at least one assertion that runs on BOTH branches. Two of the tests below need no viewport at
// all and are the ones that cannot go quiet.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"

namespace
{
    // File-unique prefix. Unity merges these translation units, and TestCameraFrameHandlers.cpp
    // already owns PWCam*; a same-named anonymous helper here would be an ODR collision.
    const TCHAR* PWCamSubjEngineCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // Every size in this file is 256 on purpose, matching the size the neighbouring camera tests
    // already use. A capture size that VARIES within one editor session trips
    // `Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in FViewport::GetHitProxy --
    // constant size is proven safe over 48 back-to-back cycles, and introducing a fourth size into
    // this suite would be adding a new instance of a hazard that has already cost unsaved level
    // state on this project.
    constexpr int32 PWCamSubjCaptureEdge = 256;

    // The typed, non-crashing exits a camera capture is allowed to take when it cannot complete in
    // this run. Same list as TestCameraFrameHandlers.cpp's PWCamIsTypedCaptureFailure, extended by
    // the codes a SUBJECT acquisition can add on a machine that cannot open an asset editor:
    // OPEN_FAILED, PREVIEW_NOT_FOUND, SUBSYSTEM_MISSING and UNSUPPORTED_ASSET_EDITOR.
    //
    // TWO codes are deliberately ABSENT because they would be real defects rather than
    // environment skips. ACTOR_NOT_FOUND: the fixture spawns non-transient, so an unresolved actor
    // is a bug (the same reasoning TestCameraFrameHandlers.cpp records). ASSET_NOT_FOUND: every
    // asset-subject test below loads its asset first and skips if it is missing, so reaching the
    // verb and being told the asset does not exist means the resolver lost the path.
    bool PWCamSubjIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            ErrorCode == TEXT("PREVIEW_NOT_FOUND") ||
            ErrorCode == TEXT("OPEN_FAILED") ||
            ErrorCode == TEXT("SUBSYSTEM_MISSING") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED") ||
            ErrorCode == TEXT("UNSUPPORTED_ASSET_EDITOR") ||
            ErrorCode == TEXT("BOUNDS_EMPTY");
    }

    // Non-transient cube fixture, same rationale as PWCamSpawnResolvableCubeActor in
    // TestCameraFrameHandlers.cpp: both verbs resolve their legacy target through
    // McpActorUtils::FindActorByName -> UEditorActorSubsystem::GetAllLevelActors, which drops
    // RF_Transient actors, so a transient probe is permanently invisible to the verb under test.
    AStaticMeshActor* PWCamSubjSpawnCube(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, PWCamSubjEngineCubePath);
        if (!CubeMesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams; // deliberately NOT RF_Transient
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    void PWCamSubjDeleteFile(const FString& Path)
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    // Collect every `path` out of a shots array so the test can delete what it wrote.
    void PWCamSubjCollectShotPaths(const TArray<TSharedPtr<FJsonValue>>& Shots, TArray<FString>& OutPaths)
    {
        for (const TSharedPtr<FJsonValue>& ShotVal : Shots)
        {
            const TSharedPtr<FJsonObject>* ShotObj = nullptr;
            if (ShotVal.IsValid() && ShotVal->TryGetObject(ShotObj) && ShotObj && (*ShotObj).IsValid())
            {
                FString Path;
                if ((*ShotObj)->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
                {
                    OutPaths.AddUnique(Path);
                }
            }
        }
    }
}

// ============================================================================
// The pose bound. No viewport needed, and this is the assertion that cannot go
// quiet: it pins the fact the conversion onto the shared primitive is most likely
// to lose silently.
// ============================================================================

// `PoseRequest.MaxPoses = GMaxOrbitShots` is one line in CameraFrameHandler.cpp, and deleting it
// is invisible -- the verb keeps working and every set just gets shorter, because the primitive's
// own default is 8. The two numbers differing is what makes that line load-bearing; asserting
// them here means a future edit that "simplifies" the line away fails a test rather than quietly
// truncating sets callers have been taking for months.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitPoseBoundIsTheVerbsOwnTest,
    "PinWright.camera.orbit_shots.PoseBoundIsTheVerbsOwnNotThePrimitives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitPoseBoundIsTheVerbsOwnTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("camera.orbit_shots' own ceiling is still 24"),
        PinWrightCameraFrame::GMaxOrbitShots, 24);
    TestEqual(TEXT("the shared primitive's default ceiling is still 8"),
        PinWrightPoseCapture::MaxPosesPerCall, 8);
    // The precondition that makes the live test below meaningful: if these were equal, a dropped
    // MaxPoses assignment would change nothing and a 24-shot set would pass either way.
    TestTrue(TEXT("the verb's ceiling and the primitive's default differ, so the assignment matters"),
        PinWrightCameraFrame::GMaxOrbitShots != PinWrightPoseCapture::MaxPosesPerCall);

    // The bound is enforced BEFORE any viewport is acquired, so both sides of it are assertable
    // headless. One over the ceiling is refused by name...
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TEXT("PW_NoSuchActorForBoundsCheck"));
        Payload->SetNumberField(TEXT("count"), PinWrightCameraFrame::GMaxOrbitShots + 1);

        FTestResponseCapture Capture;
        TestTrue(TEXT("camera.orbit_shots handler found"),
            InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
        TestFalse(TEXT("one shot over the ceiling is refused"), Capture.bSuccess);
        TestEqual(TEXT("over-ceiling refusal is TOO_MANY_SHOTS"), Capture.ErrorCode,
            FString(TEXT("TOO_MANY_SHOTS")));
    }

    // ... and exactly the ceiling is NOT, so the plan reached the target-resolution step. The
    // fixture actor deliberately does not exist, so the next refusal is ACTOR_NOT_FOUND: that is
    // the proof the 24-shot plan was accepted, without needing a viewport to prove it.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TEXT("PW_NoSuchActorForBoundsCheck"));
        Payload->SetNumberField(TEXT("count"), PinWrightCameraFrame::GMaxOrbitShots);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture);
        TestFalse(TEXT("the unresolvable fixture still fails"), Capture.bSuccess);
        TestNotEqual(TEXT("a set of exactly the ceiling is not refused as too many"),
            Capture.ErrorCode, FString(TEXT("TOO_MANY_SHOTS")));
        TestEqual(TEXT("it fails on the target instead, so the plan was accepted"),
            Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    }
    return true;
}

// ============================================================================
// The 24-shot set end to end (RHI-guarded).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitSetStillAllowsTwentyFourTest,
    "PinWright.camera.orbit_shots.OrbitSetStillAllowsTwentyFour",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitSetStillAllowsTwentyFourTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamSubj24_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSubjSpawnCube(World, Label);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Payload->SetNumberField(TEXT("count"), PinWrightCameraFrame::GMaxOrbitShots);
    Payload->SetNumberField(TEXT("width"), PWCamSubjCaptureEdge);
    Payload->SetNumberField(TEXT("height"), PWCamSubjCaptureEdge);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        TestTrue(TEXT("capture failure is typed"), PWCamSubjIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            FString::Printf(
                TEXT("Skipped the 24-shot assertions: live viewport capture unavailable in this run (%s). "
                     "PoseBoundIsTheVerbsOwnNotThePrimitives covers the bound without a viewport."),
                *Capture.ErrorCode));
        return true;
    }

    TestEqual(TEXT("a 24-shot request returns 24 shots"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("count"))),
        PinWrightCameraFrame::GMaxOrbitShots);

    const TSharedPtr<FJsonObject>* PoseSet = nullptr;
    if (Capture.Result->TryGetObjectField(TEXT("poseSet"), PoseSet) && PoseSet && (*PoseSet).IsValid())
    {
        TestEqual(TEXT("nothing was truncated"),
            static_cast<int32>((*PoseSet)->GetNumberField(TEXT("posesTruncated"))), 0);
        // The bound in force, published rather than implicit. 8 here would be the primitive's
        // default leaking through -- exactly the silent shortening this test exists for.
        TestEqual(TEXT("the bound in force is the verb's own 24, not the primitive's 8"),
            static_cast<int32>((*PoseSet)->GetNumberField(TEXT("maxPosesPerCall"))),
            PinWrightCameraFrame::GMaxOrbitShots);
    }
    else
    {
        AddError(TEXT("a converted orbit set published no poseSet block"));
    }

    TArray<FString> Paths;
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots)
    {
        TestEqual(TEXT("the shots array has 24 entries"), Shots->Num(),
            PinWrightCameraFrame::GMaxOrbitShots);
        PWCamSubjCollectShotPaths(*Shots, Paths);
    }
    for (const FString& Path : Paths)
    {
        PWCamSubjDeleteFile(Path);
    }
    return true;
}

// ============================================================================
// `subject` is a declared parameter on both verbs. No viewport needed: this
// routes through the real dispatcher, whose unknown-param gate runs before the
// handler body, so an undeclared `subject` would come back UNKNOWN_PARAMS.
//
// The subject deliberately names an asset that does not exist. That keeps the
// test to what it claims to test: the resolver refuses before anything opens an
// asset editor window or writes a PNG, so this leaves no window behind for the
// ~FStaticMeshEditor shutdown fault to find and no files for a later test to trip
// over -- while still proving the argument reached the handler.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraSubjectParamIsDeclaredTest,
    "PinWright.camera.frame_actor.SubjectParamIsAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraSubjectParamIsDeclaredTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const TCHAR* Methods[] = { TEXT("camera.frame_actor"), TEXT("camera.orbit_shots") };
    for (const TCHAR* Method : Methods)
    {
        TSharedPtr<FJsonObject> SubjectObj = MakeShared<FJsonObject>();
        SubjectObj->SetStringField(TEXT("kind"), TEXT("staticMesh"));
        SubjectObj->SetStringField(TEXT("path"),
            TEXT("/Game/PW_NoSuchAsset_SchemaProbe.PW_NoSuchAsset_SchemaProbe"));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("subject"), SubjectObj);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            FString::Printf(TEXT("req-cam-subject-%s"), Method), Params, bSuccess, ErrorCode);

        // The point of the assertion is the CODE, not the outcome: a capture may well fail in a
        // headless run, but it must not fail because the schema does not know the argument.
        // TestNotEqual has no FString-`What` overload in UE 5.8 (only TestEqual does), so the
        // label is dereferenced to a TCHAR* -- the temporary lives to the end of the full
        // expression, which is all this needs.
        TestNotEqual(*FString::Printf(TEXT("%s declares `subject`"), Method),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        // Nor may `subject` alone read as "no target named" -- that is the other way this could
        // regress, and it would look identical to a caller. Unconditional: the named asset does
        // not exist, so this call cannot succeed and the assertion cannot be skipped.
        TestFalse(*FString::Printf(TEXT("%s cannot capture an asset that does not exist"), Method),
            bSuccess);
        TestNotEqual(
            *FString::Printf(TEXT("%s does not treat a bare `subject` as a missing target"), Method),
            ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
        AddInfo(FString::Printf(TEXT("%s with a bare subject: success=%s code=%s"),
            Method, bSuccess ? TEXT("true") : TEXT("false"),
            ErrorCode.IsEmpty() ? TEXT("<none>") : *ErrorCode));
    }
    return true;
}

// ============================================================================
// The refusal that MOVED. `actorName` had to stop being a required param so that
// `subject` could stand in for it, which took the "no target at all" refusal out
// of the dispatcher's gate and put it in the handler body. Same code, and the
// message must now name both ways of saying what to shoot.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFrameNoTargetNamesSubjectTest,
    "PinWright.camera.frame_actor.NoTargetNamesSubject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFrameNoTargetNamesSubjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.frame_actor handler found"),
        InvokeHandlerWithCapture(TEXT("camera.frame_actor"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("an empty payload still fails"), Capture.bSuccess);
    TestEqual(TEXT("and still fails as INVALID_ARGUMENT"), Capture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));

    // Both spellings, so the message cannot lose one of them. Asserting only that the call failed
    // would pass on a typo, which is the failure mode these substring pairs exist to catch.
    TestTrue(TEXT("the refusal names the actorName slot"),
        Capture.Message.Contains(TEXT("actorName")));
    TestTrue(TEXT("the refusal names the subject slot"),
        Capture.Message.Contains(TEXT("subject")));
    return true;
}

// ============================================================================
// Two targets in one payload is a refusal, not a ranking. No viewport needed,
// so this pair cannot go quiet either.
//
// The danger being pinned is not that the call fails -- it is that it SUCCEEDS,
// silently shooting whichever target the code happened to check first, and hands
// back a picture of the wrong thing with nothing in the response saying so.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFrameSubjectAndActorNameRefusedTest,
    "PinWright.camera.frame_actor.SubjectAndActorNameIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFrameSubjectAndActorNameRefusedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> SubjectObj = MakeShared<FJsonObject>();
    SubjectObj->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    SubjectObj->SetStringField(TEXT("path"), PWCamSubjEngineCubePath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), SubjectObj);
    Payload->SetStringField(TEXT("actorName"), TEXT("PW_SomeActorThatWouldLose"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.frame_actor handler found"),
        InvokeHandlerWithCapture(TEXT("camera.frame_actor"), Payload, Capture));
    TestFalse(TEXT("naming a subject AND an actor is refused, not ranked"), Capture.bSuccess);
    TestEqual(TEXT("the contradiction is INVALID_ARGUMENT"), Capture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    // Both key names, so the caller learns which one to remove. Asserting only that it failed
    // would pass on any unrelated refusal.
    TestTrue(TEXT("the refusal names subject"), Capture.Message.Contains(TEXT("subject")));
    TestTrue(TEXT("the refusal names actorName"), Capture.Message.Contains(TEXT("actorName")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitSubjectAndLegacyTargetRefusedTest,
    "PinWright.camera.orbit_shots.SubjectAndLegacyTargetIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitSubjectAndLegacyTargetRefusedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> SubjectObj = MakeShared<FJsonObject>();
    SubjectObj->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    SubjectObj->SetStringField(TEXT("path"), PWCamSubjEngineCubePath);

    // Against `point`, not `actorName`: the two legacy spellings take different branches, and the
    // point branch is the one that would otherwise have been reached first.
    TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
    Point->SetNumberField(TEXT("x"), 0.0);
    Point->SetNumberField(TEXT("y"), 0.0);
    Point->SetNumberField(TEXT("z"), 0.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), SubjectObj);
    Payload->SetObjectField(TEXT("point"), Point);
    Payload->SetNumberField(TEXT("radius"), 500.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestFalse(TEXT("naming a subject AND a point is refused, not ranked"), Capture.bSuccess);
    TestEqual(TEXT("the contradiction is INVALID_ARGUMENT"), Capture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the refusal names subject"), Capture.Message.Contains(TEXT("subject")));
    TestTrue(TEXT("the refusal names point"), Capture.Message.Contains(TEXT("point")));
    return true;
}

// ============================================================================
// The single still through the shared primitive: it must inherit the throwaway
// warm-up frame, publish a `framing` verdict, and publish `resolutionSource` in
// the shared vocabulary -- and must NOT publish a `subject` block, because the
// caller named a placed actor and gave it no subject.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFrameSingleStillInheritsWarmupTest,
    "PinWright.camera.frame_actor.SingleStillInheritsTheWarmup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFrameSingleStillInheritsWarmupTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_CamSubjStill_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = PWCamSubjSpawnCube(World, Label);
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Payload->SetNumberField(TEXT("width"), PWCamSubjCaptureEdge);
    Payload->SetNumberField(TEXT("height"), PWCamSubjCaptureEdge);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.frame_actor handler found"),
        InvokeHandlerWithCapture(TEXT("camera.frame_actor"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        TestTrue(TEXT("capture failure is typed"), PWCamSubjIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            FString::Printf(
                TEXT("Skipped the warm-up / framing / subject-absence assertions: live viewport capture "
                     "unavailable in this run (%s)."), *Capture.ErrorCode));
        return true;
    }

    // The whole reason this verb was routed through the pose-list primitive: it was the one camera
    // verb that never discarded a cold first frame.
    const TSharedPtr<FJsonObject>* PoseSet = nullptr;
    if (Capture.Result->TryGetObjectField(TEXT("poseSet"), PoseSet) && PoseSet && (*PoseSet).IsValid())
    {
        bool bWarmupTaken = false;
        TestTrue(TEXT("poseSet reports whether the warm-up frame was taken"),
            (*PoseSet)->TryGetBoolField(TEXT("warmupShotTaken"), bWarmupTaken));
        TestTrue(TEXT("a single still now takes the throwaway warm-up frame"), bWarmupTaken);
        TestEqual(TEXT("exactly one pose was captured"),
            static_cast<int32>((*PoseSet)->GetNumberField(TEXT("posesCaptured"))), 1);
    }
    else
    {
        AddError(TEXT("camera.frame_actor published no poseSet block, so it did not go through the "
                      "pose-list primitive and did not take a warm-up frame"));
    }

    // Was the cube actually in the picture? `blank` cannot answer that -- a lit backdrop is not a
    // black frame -- which is the whole reason this block exists.
    const TSharedPtr<FJsonObject>* Framing = nullptr;
    if (Capture.Result->TryGetObjectField(TEXT("framing"), Framing) && Framing && (*Framing).IsValid())
    {
        bool bEvaluated = false;
        TestTrue(TEXT("framing states whether it could be evaluated"),
            (*Framing)->TryGetBoolField(TEXT("evaluated"), bEvaluated));
        if (bEvaluated)
        {
            bool bInFrame = false;
            TestTrue(TEXT("an evaluated framing block carries a verdict"),
                (*Framing)->TryGetBoolField(TEXT("boundsInFrame"), bInFrame));
            // The absence half of the contract: a warning present on a frame that DID contain the
            // subject would be a fourteenth assert-absent case discovered from a diff.
            if (bInFrame)
            {
                TestFalse(TEXT("no framingWarning on a frame that contains the subject"),
                    (*Framing)->HasField(TEXT("framingWarning")));
            }
        }
        else
        {
            AddInfo(TEXT("framing reported evaluated=false: the fixture produced no usable bounds."));
        }
    }
    else
    {
        AddError(TEXT("camera.frame_actor published no framing block"));
    }

    // One vocabulary for resolutionSource across every capture verb. This call passed width and
    // height, so the only correct answer is "caller".
    FString ResolutionSource;
    TestTrue(TEXT("camera.frame_actor now publishes resolutionSource at all"),
        Capture.Result->TryGetStringField(TEXT("resolutionSource"), ResolutionSource));
    TestEqual(TEXT("explicit sizes read as caller-chosen, the same word orbit_shots uses"),
        ResolutionSource, FString(TEXT("caller")));

    // A caller who named a placed actor gave this verb NO subject, so the block must be absent
    // rather than present and empty. This is the assertion the plan's own risk list asks for by
    // name; it is written here, up front, rather than left to be discovered from a diff later.
    TestFalse(TEXT("no subject block on a call that named no subject"),
        Capture.Result->HasField(TEXT("subject")));

    FString CapturedPath;
    Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);
    PWCamSubjDeleteFile(CapturedPath);
    return true;
}

// ============================================================================
// An ASSET subject reaching the six-sides plan -- the cell camera.orbit_shots
// could not reach at all before, because its only targets were a placed actor
// and a bare world point.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraOrbitAssetSubjectSixSidesTest,
    "PinWright.camera.orbit_shots.AssetSubjectProducesSixSides",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraOrbitAssetSubjectSixSidesTest::RunTest(const FString& Parameters)
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, PWCamSubjEngineCubePath);
    if (!CubeMesh)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: the engine cube asset is unavailable in this run."));
        return true;
    }

    TSharedPtr<FJsonObject> SubjectObj = MakeShared<FJsonObject>();
    SubjectObj->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    SubjectObj->SetStringField(TEXT("path"), PWCamSubjEngineCubePath);
    // Explicit rather than defaulted: an asset editor left open when the editor exits faults in
    // ~FStaticMeshEditor, and a test is exactly the caller that will not be around to close it.
    SubjectObj->SetBoolField(TEXT("closeAfterCapture"), true);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), SubjectObj);
    Payload->SetStringField(TEXT("views"), TEXT("sides"));
    Payload->SetNumberField(TEXT("width"), PWCamSubjCaptureEdge);
    Payload->SetNumberField(TEXT("height"), PWCamSubjCaptureEdge);

    FTestResponseCapture Capture;
    TestTrue(TEXT("camera.orbit_shots handler found"),
        InvokeHandlerWithCapture(TEXT("camera.orbit_shots"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        TestTrue(TEXT("an asset-subject failure is typed"),
            PWCamSubjIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(
                TEXT("Skipped the six-sides assertions: the static-mesh subject could not be acquired in "
                     "this run (%s). An asset-editor preview is not lit under -unattended and may not "
                     "open at all, so this is an environment skip, not a pass."), *Capture.ErrorCode));
        return true;
    }

    TestEqual(TEXT("an asset subject with views:'sides' returns 6 shots"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("count"))), 6);

    // The six pairs, as LITERALS. Comparing against MakeSideViews would be circular -- if the
    // shared table were reordered or re-spelled, both sides of the comparison would move together
    // and the test would still pass. `back` is 180 rather than -180 and `right` is -90 rather than
    // 270 because FRotator::NormalizeAxis returns those, and a different spelling would make the
    // orthographic snap report a move that never happened.
    const float ExpectedAzimuth[6]   = {   0.0f, 180.0f,  90.0f, -90.0f,   0.0f,   0.0f };
    const float ExpectedElevation[6] = {   0.0f,   0.0f,   0.0f,   0.0f,  90.0f, -90.0f };

    TArray<FString> Paths;
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots)
    {
        TestEqual(TEXT("the shots array has 6 entries"), Shots->Num(), 6);
        for (int32 Index = 0; Index < Shots->Num() && Index < 6; ++Index)
        {
            const TSharedPtr<FJsonObject>* ShotObj = nullptr;
            if (!(*Shots)[Index].IsValid() || !(*Shots)[Index]->TryGetObject(ShotObj) ||
                !ShotObj || !(*ShotObj).IsValid())
            {
                AddError(FString::Printf(TEXT("shot %d is not an object"), Index));
                continue;
            }
            const TSharedPtr<FJsonObject>* AngleObj = nullptr;
            if (!(*ShotObj)->TryGetObjectField(TEXT("angle"), AngleObj) || !AngleObj ||
                !(*AngleObj).IsValid())
            {
                AddError(FString::Printf(TEXT("shot %d carries no angle object"), Index));
                continue;
            }
            TestEqual(FString::Printf(TEXT("shot %d azimuth"), Index),
                static_cast<float>((*AngleObj)->GetNumberField(TEXT("azimuth"))),
                ExpectedAzimuth[Index]);
            TestEqual(FString::Printf(TEXT("shot %d elevation"), Index),
                static_cast<float>((*AngleObj)->GetNumberField(TEXT("elevation"))),
                ExpectedElevation[Index]);
            // These six poses land exactly on a cardinal axis, so the orthographic snap is a no-op
            // and must report nothing. A reported snap here means the table was re-spelled.
            TestFalse(FString::Printf(TEXT("shot %d needed no orthographic snap"), Index),
                (*ShotObj)->HasField(TEXT("orthoAxisSnapped")));
        }
        PWCamSubjCollectShotPaths(*Shots, Paths);
    }
    else
    {
        AddError(TEXT("no shots array on a successful asset-subject set"));
    }

    // The other half of the emit-only-when-resolved rule: a call that DID name a subject must
    // carry the block. Asserting only the absent case would pass on a verb that never emits it.
    const TSharedPtr<FJsonObject>* SubjectBlock = nullptr;
    if (Capture.Result->TryGetObjectField(TEXT("subject"), SubjectBlock) && SubjectBlock &&
        (*SubjectBlock).IsValid())
    {
        FString Kind;
        (*SubjectBlock)->TryGetStringField(TEXT("kind"), Kind);
        TestEqual(TEXT("the subject block reports the kind that was resolved"), Kind,
            FString(TEXT("staticMesh")));
    }
    else
    {
        AddError(TEXT("a resolved subject published no subject block"));
    }

    for (const FString& Path : Paths)
    {
        PWCamSubjDeleteFile(Path);
    }
    return true;
}
