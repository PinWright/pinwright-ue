// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the level-prototyping ergonomics actor verbs:
// actor.spawn_shape, actor.spawn_batch, actor.set_folder, actor.nudge.
// Each spawning test uses FScopedEditorWorldActorGuard so actors it places into the
// open map (L_Core) are destroyed and the level's dirty flag restored on scope exit,
// leaving the map exactly as found.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Utils/ActorUtils.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // Total actors in the editor world, or -1 if there is no world. Uniquely named to
    // avoid an ODR clash with sibling test files' count helpers under a Unity merge.
    int32 CountErgoWorldActors()
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

    // Builds one transforms[] entry {location:{x,y,z}} at the given location.
    TSharedPtr<FJsonValue> MakeTransformEntry(const FVector& Location)
    {
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), Location.X);
        Loc->SetNumberField(TEXT("y"), Location.Y);
        Loc->SetNumberField(TEXT("z"), Location.Z);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetObjectField(TEXT("location"), Loc);
        return MakeShared<FJsonValueObject>(Entry);
    }

    FString MakeErgoLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

// ============================================================================
// actor.spawn_shape
// ============================================================================

// CUBE spawns exactly one StaticMeshActor carrying the engine Cube mesh.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnShapeCubeTest,
    "PinWright.actor.spawn_shape.Cube",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnShapeCubeTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn_shape.Cube."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const int32 Before = CountErgoWorldActors();
    const FString Label = MakeErgoLabel(TEXT("PW_ShapeCube"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("cube"));
    Payload->SetStringField(TEXT("actorName"), Label);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_shape is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), Payload, Capture));
    TestTrue(TEXT("actor.spawn_shape CUBE succeeds"), Capture.bSuccess);

    TestEqual(TEXT("exactly one actor was created"), CountErgoWorldActors(), Before + 1);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString MeshPath;
        TestTrue(TEXT("response carries meshPath"), Capture.Result->TryGetStringField(TEXT("meshPath"), MeshPath));
        TestTrue(TEXT("meshPath points at the engine Cube"), MeshPath.Contains(TEXT("BasicShapes/Cube")));
    }

    AStaticMeshActor* Spawned = Cast<AStaticMeshActor>(McpActorUtils::FindActorByName(nullptr, Label));
    TestNotNull(TEXT("spawned StaticMeshActor found by label"), Spawned);
    if (Spawned && Spawned->GetStaticMeshComponent())
    {
        UStaticMesh* Mesh = Spawned->GetStaticMeshComponent()->GetStaticMesh();
        TestNotNull(TEXT("spawned actor has a static mesh assigned"), Mesh);
        if (Mesh)
        {
            TestEqual(TEXT("assigned mesh is the Cube"), Mesh->GetName(), FString(TEXT("Cube")));
        }
    }
    return true;
}

// An unrecognized shape token is rejected with a typed INVALID_PARAMS (no actor spawned).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnShapeInvalidTest,
    "PinWright.actor.spawn_shape.InvalidShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnShapeInvalidTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("PYRAMID"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_shape is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), Payload, Capture));
    TestFalse(TEXT("unknown shape is not a success"), Capture.bSuccess);
    TestEqual(TEXT("unknown shape yields INVALID_PARAMS"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

// An empty shape is rejected with a typed INVALID_PARAMS.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnShapeMissingTest,
    "PinWright.actor.spawn_shape.MissingShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnShapeMissingTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT(""));

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_shape is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), Payload, Capture));
    TestFalse(TEXT("missing shape is not a success"), Capture.bSuccess);
    TestEqual(TEXT("missing shape yields INVALID_PARAMS"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

// ============================================================================
// actor.spawn_batch
// ============================================================================

// Three transforms from a shape source yield three actors.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnBatchThreeTest,
    "PinWright.actor.spawn_batch.ThreeTransforms",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnBatchThreeTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn_batch.ThreeTransforms."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const int32 Before = CountErgoWorldActors();

    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(MakeTransformEntry(FVector(0, 0, 0)));
    Transforms.Add(MakeTransformEntry(FVector(200, 0, 0)));
    Transforms.Add(MakeTransformEntry(FVector(400, 0, 0)));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Payload->SetArrayField(TEXT("transforms"), Transforms);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_batch is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_batch"), Payload, Capture));
    TestTrue(TEXT("actor.spawn_batch succeeds"), Capture.bSuccess);

    TestEqual(TEXT("three actors were created"), CountErgoWorldActors(), Before + 3);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Count = 0.0;
        TestTrue(TEXT("response carries count"), Capture.Result->TryGetNumberField(TEXT("count"), Count));
        TestEqual(TEXT("response count is 3"), (int32)Count, 3);

        const TArray<TSharedPtr<FJsonValue>>* Spawned = nullptr;
        TestTrue(TEXT("response carries spawned array"), Capture.Result->TryGetArrayField(TEXT("spawned"), Spawned));
        if (Spawned)
        {
            TestEqual(TEXT("spawned array has 3 entries"), Spawned->Num(), 3);
        }
    }
    return true;
}

// A batch with no source is rejected with a typed INVALID_PARAMS.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnBatchNoSourceTest,
    "PinWright.actor.spawn_batch.MissingSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnBatchNoSourceTest::RunTest(const FString& Parameters)
{
    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(MakeTransformEntry(FVector(0, 0, 0)));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("transforms"), Transforms);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_batch is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_batch"), Payload, Capture));
    TestFalse(TEXT("missing source is not a success"), Capture.bSuccess);
    TestEqual(TEXT("missing source yields INVALID_PARAMS"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

// A batch with no transforms is rejected with a typed INVALID_PARAMS.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnBatchNoTransformsTest,
    "PinWright.actor.spawn_batch.MissingTransforms",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnBatchNoTransformsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_batch is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_batch"), Payload, Capture));
    TestFalse(TEXT("missing transforms is not a success"), Capture.bSuccess);
    TestEqual(TEXT("missing transforms yields INVALID_PARAMS"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

// Malformed transforms[] entries are REPORTED, not silently dropped: the response
// carries skippedCount plus a skipped[] array of {index, reason}. On the pre-fix code
// the two bad entries vanished and the caller could only infer the loss from
// count < requested, so the skippedCount/skipped assertions below fail if reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnBatchReportsSkippedTest,
    "PinWright.actor.spawn_batch.ReportsSkippedEntries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnBatchReportsSkippedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn_batch.ReportsSkippedEntries."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // index 0 valid, index 1 a bare number, index 2 a string: two malformed entries.
    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(MakeTransformEntry(FVector(0, 0, 0)));
    Transforms.Add(MakeShared<FJsonValueNumber>(42.0));
    Transforms.Add(MakeShared<FJsonValueString>(TEXT("not-a-transform")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Payload->SetArrayField(TEXT("transforms"), Transforms);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_batch is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_batch"), Payload, Capture));
    TestTrue(TEXT("a partially-malformed batch still succeeds"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Count = 0.0;
        Capture.Result->TryGetNumberField(TEXT("count"), Count);
        TestEqual(TEXT("only the one valid entry spawned"), (int32)Count, 1);

        double Skipped = -1.0;
        TestTrue(TEXT("response carries skippedCount"),
            Capture.Result->TryGetNumberField(TEXT("skippedCount"), Skipped));
        TestEqual(TEXT("both malformed entries are counted as skipped"), (int32)Skipped, 2);

        double Requested = 0.0;
        Capture.Result->TryGetNumberField(TEXT("requested"), Requested);
        TestEqual(TEXT("count + skippedCount accounts for every requested entry"),
            (int32)Count + (int32)Skipped, (int32)Requested);

        const TArray<TSharedPtr<FJsonValue>>* SkippedArr = nullptr;
        TestTrue(TEXT("response carries the skipped detail array"),
            Capture.Result->TryGetArrayField(TEXT("skipped"), SkippedArr));
        if (SkippedArr)
        {
            TestEqual(TEXT("skipped array has one entry per dropped transform"), SkippedArr->Num(), 2);
            const TSharedPtr<FJsonObject>* First = nullptr;
            if (SkippedArr->Num() > 0 && (*SkippedArr)[0].IsValid()
                && (*SkippedArr)[0]->TryGetObject(First) && First)
            {
                double Index = -1.0;
                TestTrue(TEXT("skipped entry carries its source index"),
                    (*First)->TryGetNumberField(TEXT("index"), Index));
                TestEqual(TEXT("the first skipped entry is index 1"), (int32)Index, 1);
                FString Reason;
                TestTrue(TEXT("skipped entry carries a reason"),
                    (*First)->TryGetStringField(TEXT("reason"), Reason));
                TestFalse(TEXT("the reason is not empty"), Reason.IsEmpty());
            }
        }
    }
    return true;
}

// A clean batch still reports skippedCount:0 and omits the detail array entirely.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnBatchCleanBatchHasNoSkipDetailTest,
    "PinWright.actor.spawn_batch.CleanBatchReportsZeroSkipped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnBatchCleanBatchHasNoSkipDetailTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn_batch.CleanBatchReportsZeroSkipped."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(MakeTransformEntry(FVector(0, 0, 0)));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Payload->SetArrayField(TEXT("transforms"), Transforms);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_batch is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_batch"), Payload, Capture));
    TestTrue(TEXT("clean batch succeeds"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Skipped = -1.0;
        TestTrue(TEXT("skippedCount is always present"),
            Capture.Result->TryGetNumberField(TEXT("skippedCount"), Skipped));
        TestEqual(TEXT("a clean batch skips nothing"), (int32)Skipped, 0);
        TestFalse(TEXT("no skipped detail array on a clean batch"),
            Capture.Result->HasField(TEXT("skipped")));
    }
    return true;
}

// ============================================================================
// actor.set_folder
// ============================================================================

// set_folder assigns the actor's World Outliner folder path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetFolderTest,
    "PinWright.actor.set_folder.SetsFolder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetFolderTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.set_folder.SetsFolder."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = MakeErgoLabel(TEXT("PW_FolderCube"));

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    SpawnPayload->SetStringField(TEXT("actorName"), Label);
    FTestResponseCapture SpawnCapture;
    InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), SpawnPayload, SpawnCapture);
    if (!SpawnCapture.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("spawn-shape-failed"),
            TEXT("spawn_shape failed; skipping set_folder assertions."));
        return true;
    }

    const FString FolderPath = TEXT("Prototype/Walls");
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("folderPath"), FolderPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_folder is registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_folder"), Payload, Capture));
    TestTrue(TEXT("actor.set_folder succeeds"), Capture.bSuccess);

    AActor* Actor = McpActorUtils::FindActorByName(nullptr, Label);
    TestNotNull(TEXT("actor found after set_folder"), Actor);
    if (Actor)
    {
        TestEqual(TEXT("actor's folder path was set"), Actor->GetFolderPath().ToString(), FolderPath);
    }
    return true;
}

// The key a caller reads off the response is the key the next call accepts.
// set_folder emits `folder` (top level and per row) but used to REQUIRE `folderPath`,
// so replaying the verb's own output was refused by the dispatcher's required-param
// gate with MISSING_REQUIRED_PARAM. Both calls go through the real dispatcher because
// InvokeHandlerWithCapture bypasses that gate, which is exactly where the defect lived.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetFolderAliasRoundTripTest,
    "PinWright.actor.set_folder.FolderAliasRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetFolderAliasRoundTripTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() is null; the round trip needs a placed actor."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = MakeErgoLabel(TEXT("PW_FolderAliasCube"));

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    SpawnPayload->SetStringField(TEXT("actorName"), Label);
    FTestResponseCapture SpawnCapture;
    InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), SpawnPayload, SpawnCapture);
    if (!SpawnCapture.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("spawn-shape-failed"),
            TEXT("actor.spawn_shape did not place the probe cube; the folder round trip was not measured."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Call 1: the canonical spelling still works, and the response echoes `folder`.
    const FString FirstFolder = TEXT("Prototype/AliasRoundTrip/First");
    TSharedPtr<FJsonObject> FirstParams = MakeShared<FJsonObject>();
    FirstParams->SetStringField(TEXT("actorName"), Label);
    FirstParams->SetStringField(TEXT("folderPath"), FirstFolder);

    bool bFirstSuccess = false;
    TSharedPtr<FJsonObject> FirstResult;
    FString FirstErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("actor.set_folder"),
        TEXT("req-set-folder-alias-first"), FirstParams, bFirstSuccess, FirstResult, FirstErrorCode);
    TestTrue(TEXT("folderPath is still accepted"), bFirstSuccess);

    FString EchoedKeyValue;
    if (!TestTrue(TEXT("response echoes a top-level 'folder' key"),
            FirstResult.IsValid() && FirstResult->TryGetStringField(TEXT("folder"), EchoedKeyValue)))
    {
        return true;
    }
    TestEqual(TEXT("the echoed folder is the folder that was assigned"), EchoedKeyValue, FirstFolder);

    // Call 2: feed the response's own key name back in, carrying a new value so the
    // assertion cannot pass on the first call's leftover state.
    const FString SecondFolder = TEXT("Prototype/AliasRoundTrip/Second");
    TSharedPtr<FJsonObject> SecondParams = MakeShared<FJsonObject>();
    SecondParams->SetStringField(TEXT("actorName"), Label);
    SecondParams->SetStringField(TEXT("folder"), SecondFolder);

    bool bSecondSuccess = false;
    FString SecondErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("actor.set_folder"),
        TEXT("req-set-folder-alias-second"), SecondParams, bSecondSuccess, SecondErrorCode);
    TestTrue(TEXT("the response's own 'folder' key is accepted on the next call"), bSecondSuccess);
    TestEqual(TEXT("the alias payload is not refused by the param gate"),
        SecondErrorCode, FString());

    AActor* Actor = McpActorUtils::FindActorByName(nullptr, Label);
    TestNotNull(TEXT("actor found after the alias call"), Actor);
    if (Actor)
    {
        TestEqual(TEXT("the alias actually moved the actor"),
            Actor->GetFolderPath().ToString(), SecondFolder);
    }
    return true;
}

// ============================================================================
// actor.nudge
// ============================================================================

// nudge deltaWorld moves the actor by exactly the requested delta.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorNudgeDeltaWorldTest,
    "PinWright.actor.nudge.DeltaWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorNudgeDeltaWorldTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.nudge.DeltaWorld."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = MakeErgoLabel(TEXT("PW_NudgeCube"));

    // Spawn a cube at a known location.
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), 100.0);
    Loc->SetNumberField(TEXT("y"), 200.0);
    Loc->SetNumberField(TEXT("z"), 300.0);
    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    SpawnPayload->SetStringField(TEXT("actorName"), Label);
    SpawnPayload->SetObjectField(TEXT("location"), Loc);
    FTestResponseCapture SpawnCapture;
    InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), SpawnPayload, SpawnCapture);
    if (!SpawnCapture.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("spawn-shape-failed"),
            TEXT("spawn_shape failed; skipping nudge assertions."));
        return true;
    }

    AActor* Actor = McpActorUtils::FindActorByName(nullptr, Label);
    TestNotNull(TEXT("cube spawned for nudge"), Actor);
    if (!Actor)
    {
        return true;
    }
    const FVector StartLoc = Actor->GetActorLocation();

    TSharedPtr<FJsonObject> Delta = MakeShared<FJsonObject>();
    Delta->SetNumberField(TEXT("x"), 10.0);
    Delta->SetNumberField(TEXT("y"), 20.0);
    Delta->SetNumberField(TEXT("z"), 30.0);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetObjectField(TEXT("deltaWorld"), Delta);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.nudge is registered"),
        InvokeHandlerWithCapture(TEXT("actor.nudge"), Payload, Capture));
    TestTrue(TEXT("actor.nudge succeeds"), Capture.bSuccess);

    const FVector Expected = StartLoc + FVector(10.0, 20.0, 30.0);
    TestTrue(TEXT("actor moved by exactly the requested delta"),
        Actor->GetActorLocation().Equals(Expected, 0.01));
    return true;
}

// nudge on a missing actor returns a typed ACTOR_NOT_FOUND.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorNudgeMissingActorTest,
    "PinWright.actor.nudge.MissingActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorNudgeMissingActorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Delta = MakeShared<FJsonObject>();
    Delta->SetNumberField(TEXT("x"), 1.0);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), MakeErgoLabel(TEXT("PW_DoesNotExist")));
    Payload->SetObjectField(TEXT("deltaWorld"), Delta);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.nudge is registered"),
        InvokeHandlerWithCapture(TEXT("actor.nudge"), Payload, Capture));
    TestFalse(TEXT("nudge on missing actor is not a success"), Capture.bSuccess);
    TestEqual(TEXT("missing actor yields ACTOR_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

// nudge with no delta at all is rejected with a typed INVALID_PARAMS.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorNudgeNoDeltaTest,
    "PinWright.actor.nudge.MissingDelta",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorNudgeNoDeltaTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.nudge.MissingDelta."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = MakeErgoLabel(TEXT("PW_NudgeNoDelta"));

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    SpawnPayload->SetStringField(TEXT("actorName"), Label);
    FTestResponseCapture SpawnCapture;
    InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), SpawnPayload, SpawnCapture);
    if (!SpawnCapture.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("spawn-shape-failed"),
            TEXT("spawn_shape failed; skipping nudge no-delta assertion."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.nudge is registered"),
        InvokeHandlerWithCapture(TEXT("actor.nudge"), Payload, Capture));
    TestFalse(TEXT("nudge with no delta is not a success"), Capture.bSuccess);
    TestEqual(TEXT("no delta yields INVALID_PARAMS"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

// ============================================================================
// Unknown-argument rejection (through the dispatcher's param validation)
// ============================================================================

// A bogus argument to actor.spawn_shape is rejected as UNKNOWN_PARAMS before the
// handler body runs, so no actor is spawned. Routed through the real dispatcher
// (InvokeHandlerWithCapture bypasses param validation).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnShapeUnknownArgTest,
    "PinWright.actor.spawn_shape.UnknownArgRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnShapeUnknownArgTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Params->SetNumberField(TEXT("bogusArgument"), 1);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("actor.spawn_shape"),
        TEXT("req-spawn-shape-unknown-arg"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown arg is not a success"), bSuccess);
    TestEqual(TEXT("unknown arg yields UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}
