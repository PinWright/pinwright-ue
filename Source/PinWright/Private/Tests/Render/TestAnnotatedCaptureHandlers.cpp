// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for render.capture_annotated.
//
// The verb drives the LIVE Level Editor viewport through CaptureEditorViewportToPng,
// which is only reachable when a real active viewport exists. Under headless /
// -unattended automation there may be no active level viewport, so the live-capture
// test follows the same RHI-guard pattern as TestRenderHandlers.cpp / the camera
// tests: a success asserts the annotated PNG exists on disk (and has color variation);
// a failure must be TYPED (CAPTURE_FAILED / NO_ACTIVE_LEVEL_VIEWPORT / ...) and the
// pixel/shape assertions are skipped. Argument-validation and unknown-param tests need
// no viewport and run deterministically.
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

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    int32 AnnotatedCountEditorWorldActors()
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

    // The typed, non-crashing capture/viewport exits render.capture_annotated is allowed
    // to return when no live viewport can be captured (or the base frame cannot be
    // read back / re-encoded).
    bool AnnotatedIsTypedCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            ErrorCode == TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION") ||
            ErrorCode == TEXT("DECODE_FAILED") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED");
    }

    // Counts distinct colors in a saved PNG, bailing out early past a blank-frame
    // threshold. A correctly annotated frame (scene render + colored overlays) has many
    // distinct colors; a fully uniform frame has ~1. Returns false when the file cannot
    // be decoded so the caller can treat it as "not checked".
    bool AnnotatedImageHasColorVariation(const FString& PngPath, int32& OutDistinct)
    {
        OutDistinct = 0;
        FImage Loaded;
        if (!FImageUtils::LoadImage(*PngPath, Loaded))
        {
            return false;
        }
        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
        if (Pixels.Num() == 0)
        {
            return false;
        }
        TSet<uint32> Distinct;
        Distinct.Reserve(16);
        for (const FColor& C : Pixels)
        {
            Distinct.Add(C.ToPackedARGB());
            if (Distinct.Num() > 8)
            {
                break;
            }
        }
        OutDistinct = Distinct.Num();
        return OutDistinct > 1;
    }

    void AnnotatedDeleteFileIfPresent(const FString& Path)
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    }

    // Spawns a NON-transient AStaticMeshActor carrying the engine unit cube at Location
    // with the given editor label. Unlike the shared SpawnTransientCubeActor helper, the
    // fixture is a real level-registered actor: render.capture_annotated resolves each
    // bounds target through McpActorUtils::FindActorByName ->
    // UEditorActorSubsystem::GetAllLevelActors, which excludes RF_Transient actors in
    // non-play worlds, so a transient fixture would be unresolvable and the bounds overlay
    // would report found=false. Returns nullptr if there is no world, the engine cube mesh
    // is unavailable, or the spawn fails. Pair with FScopedEditorWorldActorGuard to destroy
    // it and restore the level dirty flag on scope exit.
    AStaticMeshActor* AnnotatedSpawnResolvableCubeActor(UWorld* World, const FString& Label, const FVector& Location)
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
}

// ============================================================================
// render.capture_annotated — argument validation (no viewport required)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedInvalidDimensionsTest,
    "PinWright.render.capture_annotated.InvalidDimensions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedInvalidDimensionsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 0);
    Payload->SetNumberField(TEXT("height"), 128);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("invalid dimensions fail"), Capture.bSuccess);
    TestEqual(TEXT("invalid dimensions error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedInvalidProjectionModeTest,
    "PinWright.render.capture_annotated.InvalidProjectionMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedInvalidProjectionModeTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("projectionMode"), TEXT("fisheye"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("invalid projection mode fails"), Capture.bSuccess);
    TestEqual(TEXT("invalid projection mode error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// A grid with a non-positive spacing/extent is a caller error, rejected before any
// viewport work (so this is deterministic, viewport or not).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedInvalidGridTest,
    "PinWright.render.capture_annotated.InvalidGrid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedInvalidGridTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
    Grid->SetNumberField(TEXT("spacing"), 0.0);
    Grid->SetNumberField(TEXT("extent"), 1000.0);
    Payload->SetObjectField(TEXT("grid"), Grid);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("zero grid spacing fails"), Capture.bSuccess);
    TestEqual(TEXT("zero grid spacing error"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// Unknown params are rejected by the dispatcher (before the handler body), so this
// routes through the real dispatcher rather than the direct InvokeHandler path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedUnknownParamRejectedTest,
    "PinWright.render.capture_annotated.UnknownParamRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedUnknownParamRejectedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetBoolField(TEXT("axes"), true);
    Params->SetStringField(TEXT("bogusUnknownArg"), TEXT("x"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.capture_annotated"),
        TEXT("req-annotated-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param rejected"), bSuccess);
    TestEqual(TEXT("unknown param error is UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// The full base-capture arg set must be accepted (declared in RPC_PARAMS): a payload
// carrying every base arg plus every overlay arg must not be rejected as UNKNOWN_PARAMS.
// It may still fail with a TYPED capture failure when no live viewport exists.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedKnownParamsAcceptedTest,
    "PinWright.render.capture_annotated.KnownParamsAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedKnownParamsAcceptedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("filename"), TEXT("pw_annotated_known"));
    Params->SetNumberField(TEXT("width"), 128);
    Params->SetNumberField(TEXT("height"), 128);
    Params->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    Params->SetNumberField(TEXT("fov"), 60);
    Params->SetNumberField(TEXT("orthoWidth"), 512);
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), -300); Loc->SetNumberField(TEXT("y"), 0); Loc->SetNumberField(TEXT("z"), 120);
    Params->SetObjectField(TEXT("location"), Loc);
    TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
    Rot->SetNumberField(TEXT("pitch"), -15); Rot->SetNumberField(TEXT("yaw"), 0); Rot->SetNumberField(TEXT("roll"), 0);
    Params->SetObjectField(TEXT("rotation"), Rot);
    Params->SetBoolField(TEXT("axes"), true);
    Params->SetBoolField(TEXT("labels"), true);
    Params->SetBoolField(TEXT("inline"), false);
    TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
    Grid->SetNumberField(TEXT("spacing"), 100); Grid->SetNumberField(TEXT("extent"), 500);
    Params->SetObjectField(TEXT("grid"), Grid);
    TArray<TSharedPtr<FJsonValue>> Bounds;
    Bounds.Add(MakeShared<FJsonValueString>(TEXT("AnyActor")));
    Params->SetArrayField(TEXT("bounds"), Bounds);

    bool bSuccess = true;
    TSharedPtr<FJsonObject> ResultObj;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.capture_annotated"),
        TEXT("req-annotated-known"), Params, bSuccess, ResultObj, ErrorCode);

    // The decisive assertion: a fully-declared payload is never rejected as UNKNOWN_PARAMS.
    TestNotEqual(TEXT("declared params are not rejected as UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    if (!bSuccess)
    {
        TestTrue(TEXT("any failure is a typed capture failure, not a param error"),
            AnnotatedIsTypedCaptureFailure(ErrorCode));
    }
    else if (ResultObj.IsValid())
    {
        FString Path;
        if (ResultObj->TryGetStringField(TEXT("path"), Path))
        {
            AnnotatedDeleteFileIfPresent(Path);
        }
    }
    return true;
}

// ============================================================================
// actorLabels is OPT-IN: a call that does not pass it must produce a response with no actorLabels
// block and no overlays.actorLabels key at all. This is the backward-compatibility contract - the
// discovery overlay must be invisible to every existing caller - so it fails loudly if the feature
// ever starts defaulting on.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedActorLabelsDefaultOffTest,
    "PinWright.render.capture_annotated.ActorLabelsDefaultOff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedActorLabelsDefaultOffTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 128.0);
    Payload->SetNumberField(TEXT("height"), 128.0);
    Payload->SetBoolField(TEXT("axes"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    FString CapturedPath;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("path"), CapturedPath);

        // The decisive assertions.
        TestFalse(TEXT("no top-level actorLabels block without the argument"),
            Capture.Result->HasField(TEXT("actorLabels")));

        const TSharedPtr<FJsonObject>* OverlaysObj = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("overlays"), OverlaysObj) && OverlaysObj &&
            (*OverlaysObj).IsValid())
        {
            TestFalse(TEXT("no overlays.actorLabels key without the argument"),
                (*OverlaysObj)->HasField(TEXT("actorLabels")));
            // The pre-existing overlay echo is untouched by the session refactor.
            TestTrue(TEXT("overlays still echo axes"), (*OverlaysObj)->HasField(TEXT("axes")));
            TestTrue(TEXT("overlays still echo labels"), (*OverlaysObj)->HasField(TEXT("labels")));
        }
    }
    else if (!Capture.bSuccess)
    {
        TestTrue(TEXT("capture failure is typed"), AnnotatedIsTypedCaptureFailure(Capture.ErrorCode));
    }
    AnnotatedDeleteFileIfPresent(CapturedPath);
    return true;
}

// actorLabels is a DECLARED param (so the dispatcher does not reject it), an unresolvable
// className is a typed CLASS_NOT_FOUND raised BEFORE any capture, and an opted-in call returns the
// actor->pixel map with the actor.list count/totalMatches/truncated vocabulary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedActorLabelsAcceptedTest,
    "PinWright.render.capture_annotated.ActorLabelsAcceptedAndTyped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedActorLabelsAcceptedTest::RunTest(const FString& Parameters)
{
    // (a) declared-param check through the real dispatcher (the direct invoke path skips it).
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> LabelArgs = MakeShared<FJsonObject>();
        LabelArgs->SetStringField(TEXT("folder"), TEXT("Prototype"));
        LabelArgs->SetStringField(TEXT("filter"), TEXT("PW_*"));
        LabelArgs->SetNumberField(TEXT("minScreenArea"), 64.0);
        LabelArgs->SetNumberField(TEXT("maxLabels"), 5.0);
        LabelArgs->SetNumberField(TEXT("minSpacing"), 12.0);
        LabelArgs->SetBoolField(TEXT("draw"), false);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetNumberField(TEXT("width"), 128.0);
        Params->SetNumberField(TEXT("height"), 128.0);
        Params->SetObjectField(TEXT("actorLabels"), LabelArgs);

        bool bSuccess = true;
        TSharedPtr<FJsonObject> ResultObj;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.capture_annotated"),
            TEXT("req-annotated-actorlabels"), Params, bSuccess, ResultObj, ErrorCode);

        TestNotEqual(TEXT("actorLabels is a declared param"), ErrorCode,
            FString(TEXT("UNKNOWN_PARAMS")));

        if (bSuccess && ResultObj.IsValid())
        {
            const TSharedPtr<FJsonObject>* LabelsObj = nullptr;
            if (TestTrue(TEXT("an opted-in call returns the actorLabels block"),
                    ResultObj->TryGetObjectField(TEXT("actorLabels"), LabelsObj) && LabelsObj))
            {
                TestTrue(TEXT("block carries actors[]"), (*LabelsObj)->HasField(TEXT("actors")));
                TestTrue(TEXT("block carries count"), (*LabelsObj)->HasField(TEXT("count")));
                TestTrue(TEXT("block carries totalMatches"),
                    (*LabelsObj)->HasField(TEXT("totalMatches")));
                TestTrue(TEXT("block carries truncated"),
                    (*LabelsObj)->HasField(TEXT("truncated")));
                TestTrue(TEXT("block carries the dropped breakdown"),
                    (*LabelsObj)->HasField(TEXT("dropped")));
                double ResolvedCap = 0.0;
                (*LabelsObj)->TryGetNumberField(TEXT("maxLabels"), ResolvedCap);
                TestEqual(TEXT("the requested cap is echoed"), static_cast<int32>(ResolvedCap), 5);
                // draw:false must not paint, so nothing can have been drawn.
                double Drawn = -1.0;
                (*LabelsObj)->TryGetNumberField(TEXT("drawn"), Drawn);
                TestEqual(TEXT("draw:false paints nothing"), static_cast<int32>(Drawn), 0);
            }
            FString Path;
            if (ResultObj->TryGetStringField(TEXT("path"), Path))
            {
                AnnotatedDeleteFileIfPresent(Path);
            }
        }
        else if (!bSuccess)
        {
            TestTrue(TEXT("any failure is a typed capture failure, not a param error"),
                AnnotatedIsTypedCaptureFailure(ErrorCode));
        }
    }

    // (b) an unresolvable className is CLASS_NOT_FOUND, raised before any capture happens.
    {
        bSuppressLogErrors = true;

        TSharedPtr<FJsonObject> LabelArgs = MakeShared<FJsonObject>();
        LabelArgs->SetStringField(TEXT("className"), TEXT("PW_NoSuchActorClass_ZZZ"));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("width"), 128.0);
        Payload->SetNumberField(TEXT("height"), 128.0);
        Payload->SetObjectField(TEXT("actorLabels"), LabelArgs);

        FTestResponseCapture Capture;
        TestTrue(TEXT("render.capture_annotated handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
        TestFalse(TEXT("an unresolvable className is not a success"), Capture.bSuccess);
        TestEqual(TEXT("an unresolvable className yields CLASS_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
        // Pre-flight, not capture-and-warn: no PNG path came back to clean up.
        TestFalse(TEXT("no capture result on a rejected class filter"), Capture.Result.IsValid());
    }

    // (c) a negative threshold is a caller error, rejected before any viewport work.
    {
        bSuppressLogErrors = true;

        TSharedPtr<FJsonObject> LabelArgs = MakeShared<FJsonObject>();
        LabelArgs->SetNumberField(TEXT("minScreenArea"), -1.0);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("actorLabels"), LabelArgs);

        FTestResponseCapture Capture;
        TestTrue(TEXT("render.capture_annotated handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
        TestFalse(TEXT("a negative minScreenArea is not a success"), Capture.bSuccess);
        TestEqual(TEXT("a negative minScreenArea yields INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    return true;
}

// ============================================================================
// render.capture_annotated — live capture with overlays over a spawned actor
// (RHI-guarded). Also proves the actor-count invariant.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAnnotatedBoundsOverActorTest,
    "PinWright.render.capture_annotated.BoundsOverActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAnnotatedBoundsOverActorTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_Annotated_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = AnnotatedSpawnResolvableCubeActor(World, Label, FVector(0.0f, 0.0f, 0.0f));
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }
    const FString ResolvedName = Actor->GetActorLabel();

    const int32 ActorCountBefore = AnnotatedCountEditorWorldActors();

    // Frame the origin (where the cube sits) so the axes/grid/box actually project on-screen.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256.0);
    Payload->SetNumberField(TEXT("height"), 256.0);
    Payload->SetStringField(TEXT("projectionMode"), TEXT("perspective"));
    Payload->SetNumberField(TEXT("fov"), 60.0);
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), -500.0);
    Loc->SetNumberField(TEXT("y"), -500.0);
    Loc->SetNumberField(TEXT("z"), 400.0);
    Payload->SetObjectField(TEXT("location"), Loc);
    TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
    Rot->SetNumberField(TEXT("pitch"), -30.0);
    Rot->SetNumberField(TEXT("yaw"), 45.0);
    Rot->SetNumberField(TEXT("roll"), 0.0);
    Payload->SetObjectField(TEXT("rotation"), Rot);
    Payload->SetBoolField(TEXT("axes"), true);
    Payload->SetBoolField(TEXT("labels"), true);
    TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
    Grid->SetNumberField(TEXT("spacing"), 100.0);
    Grid->SetNumberField(TEXT("extent"), 500.0);
    Payload->SetObjectField(TEXT("grid"), Grid);
    TArray<TSharedPtr<FJsonValue>> Bounds;
    Bounds.Add(MakeShared<FJsonValueString>(ResolvedName));
    Payload->SetArrayField(TEXT("bounds"), Bounds);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_annotated handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    FString CapturedPath;
    if (Capture.bSuccess)
    {
        TestTrue(TEXT("success result exists"), Capture.Result.IsValid());
        if (Capture.Result.IsValid())
        {
            TestTrue(TEXT("success carries a path"),
                Capture.Result->TryGetStringField(TEXT("path"), CapturedPath));
            TestTrue(TEXT("annotated PNG exists on disk"),
                !CapturedPath.IsEmpty() && IFileManager::Get().FileExists(*CapturedPath));
            TestEqual(TEXT("capture width echoed"),
                static_cast<int32>(Capture.Result->GetNumberField(TEXT("width"))), 256);
            TestEqual(TEXT("capture height echoed"),
                static_cast<int32>(Capture.Result->GetNumberField(TEXT("height"))), 256);

            // The overlays echo must report the spawned actor as found with a size.
            const TSharedPtr<FJsonObject>* OverlaysObj = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("overlays"), OverlaysObj) && OverlaysObj && (*OverlaysObj).IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* BoundsEcho = nullptr;
                if ((*OverlaysObj)->TryGetArrayField(TEXT("bounds"), BoundsEcho) && BoundsEcho && BoundsEcho->Num() > 0)
                {
                    const TSharedPtr<FJsonObject>* First = nullptr;
                    if ((*BoundsEcho)[0]->TryGetObject(First) && First && (*First).IsValid())
                    {
                        bool bFound = false;
                        (*First)->TryGetBoolField(TEXT("found"), bFound);
                        TestTrue(TEXT("spawned bounds actor was found"), bFound);
                        TestTrue(TEXT("bounds echo carries sizeCm"), (*First)->HasField(TEXT("sizeCm")));
                    }
                }
                else
                {
                    AddError(TEXT("overlays.bounds echo missing for a requested bounds actor"));
                }
            }
            else
            {
                AddError(TEXT("success result missing overlays echo"));
            }

            // The annotated frame (scene + colored overlays) must not be a uniform blank.
            if (!CapturedPath.IsEmpty() && IFileManager::Get().FileExists(*CapturedPath))
            {
                int32 Distinct = 0;
                const bool bVaried = AnnotatedImageHasColorVariation(CapturedPath, Distinct);
                TestTrue(FString::Printf(TEXT("annotated frame is not blank (distinct=%d)"), Distinct), bVaried);
            }
        }
    }
    else
    {
        TestTrue(TEXT("capture failure is typed"), AnnotatedIsTypedCaptureFailure(Capture.ErrorCode));
        AddInfo(TEXT("Skipped pixel/overlay checks: live viewport capture unavailable in this run."));
    }
    AnnotatedDeleteFileIfPresent(CapturedPath);

    // The capture must not spawn or destroy level actors (only the guard's cube exists).
    const int32 ActorCountAfter = AnnotatedCountEditorWorldActors();
    if (ActorCountBefore >= 0 && ActorCountAfter >= 0)
    {
        TestEqual(TEXT("capture_annotated does not change the level actor count"),
            ActorCountAfter, ActorCountBefore);
    }
    return true;
}
