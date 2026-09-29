// Copyright (c) 2026 Alexander Penkin. MIT License.

// Handler-level tests for the actors[] batch form of actor.set_transform / actor.nudge and for
// actor.set_transform's lookAt. Probes are normal (non-transient) level actors, because the actor
// resolver every actor.* verb uses skips RF_Transient actors; FScopedEditorWorldActorGuard destroys
// them and restores the level's dirty flag on scope exit.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    UWorld* BatchXformEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    AStaticMeshActor* BatchXformSpawnProbe(UWorld* World, const TCHAR* Prefix, const FVector& Location)
    {
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!World || !CubeMesh)
        {
            return nullptr;
        }
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        return Actor;
    }

    TSharedPtr<FJsonObject> BatchXformVec(const FVector& V)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), V.X);
        Obj->SetNumberField(TEXT("y"), V.Y);
        Obj->SetNumberField(TEXT("z"), V.Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> BatchXformEntry(const FString& ActorName, const TCHAR* Field, const FVector& V)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actorName"), ActorName);
        Entry->SetObjectField(Field, BatchXformVec(V));
        return Entry;
    }

    TSharedPtr<FJsonObject> BatchXformPayload(const TArray<TSharedPtr<FJsonObject>>& Entries)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const TSharedPtr<FJsonObject>& Entry : Entries)
        {
            Values.Add(MakeShared<FJsonValueObject>(Entry));
        }
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("actors"), Values);
        return Payload;
    }

    // The row of results[] at Index, or null.
    TSharedPtr<FJsonObject> BatchXformRow(const TSharedPtr<FJsonObject>& Result, int32 Index)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("results"), Rows) || !Rows || !Rows->IsValidIndex(Index))
        {
            return nullptr;
        }
        return (*Rows)[Index]->AsObject();
    }
}

// Two actors moved by one call land where each entry asked, and the edit is ONE undo transaction
// holding both actors.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformBatchMovesEveryEntryTest,
    "PinWright.actor.set_transform.BatchMovesEveryEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformBatchMovesEveryEntryTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World || !GEditor->Trans)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world or transactor; skipping the batch set_transform assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* A = BatchXformSpawnProbe(World, TEXT("PW_BatchXformA"), FVector(0, 0, 0));
    AStaticMeshActor* B = BatchXformSpawnProbe(World, TEXT("PW_BatchXformB"), FVector(500, 0, 0));
    if (!TestNotNull(TEXT("probe A spawned"), A) || !TestNotNull(TEXT("probe B spawned"), B))
    {
        return false;
    }

    const FVector TargetA(100, 200, 300);
    const FVector TargetB(-400, 50, 25);
    TSharedPtr<FJsonObject> EntryB = BatchXformEntry(B->GetActorLabel(), TEXT("location"), TargetB);
    EntryB->SetObjectField(TEXT("scale"), BatchXformVec(FVector(2, 2, 2)));
    const TSharedPtr<FJsonObject> Payload = BatchXformPayload({
        BatchXformEntry(A->GetActorLabel(), TEXT("location"), TargetA), EntryB });

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.set_transform is registered"),
        InvokeHandlerWithCapture(TEXT("actor.set_transform"), Payload, Capture));
    TestTrue(TEXT("batch set_transform succeeds"), Capture.bSuccess);
    TestTrue(TEXT("A moved to its entry's location"), A->GetActorLocation().Equals(TargetA, 0.01));
    TestTrue(TEXT("B moved to its entry's location"), B->GetActorLocation().Equals(TargetB, 0.01));
    TestTrue(TEXT("B took its entry's scale"), B->GetActorScale3D().Equals(FVector(2, 2, 2), 0.001));
    TestTrue(TEXT("A kept its scale (field omitted in its entry)"), A->GetActorScale3D().Equals(FVector::OneVector, 0.001));

    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("succeededCount"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("succeededCount"))), 2);
        TestEqual(TEXT("failedCount"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("failedCount"))), 0);
        TestTrue(TEXT("success is true when every entry applied"), Capture.Result->GetBoolField(TEXT("success")));
        const TSharedPtr<FJsonObject> Row1 = BatchXformRow(Capture.Result, 1);
        if (TestNotNull(TEXT("results[1] present"), Row1.Get()))
        {
            TestEqual(TEXT("results[] is index-aligned"), Row1->GetStringField(TEXT("requestedName")), B->GetActorLabel());
            TestTrue(TEXT("results[1] reports success"), Row1->GetBoolField(TEXT("success")));
        }
    }

    const int32 Newest = GEditor->Trans->GetQueueLength() - 1;
    const FTransaction* Transaction = Newest >= 0 ? GEditor->Trans->GetTransaction(Newest) : nullptr;
    if (TestNotNull(TEXT("the batch recorded an undo transaction"), Transaction))
    {
        TestEqual(TEXT("newest transaction is the batch"), Transaction->GetContext().Title.ToString(),
            FString(TEXT("MCP: actor.set_transform")));
        TestTrue(TEXT("the one transaction holds actor A"), Transaction->ContainsObject(A));
        TestTrue(TEXT("the one transaction holds actor B"), Transaction->ContainsObject(B));
    }
    return true;
}

// An unknown actor fails only its own entry: the known one still moves, the call succeeds
// partially, and the name lands in missing[] with a typed per-entry code.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformBatchUnknownActorTest,
    "PinWright.actor.set_transform.BatchReportsUnknownActorPerEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformBatchUnknownActorTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the unknown-actor batch assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* A = BatchXformSpawnProbe(World, TEXT("PW_BatchXformKnown"), FVector(0, 0, 0));
    if (!TestNotNull(TEXT("probe spawned"), A))
    {
        return false;
    }
    const FString Missing = FString::Printf(TEXT("PW_BatchXformMissing_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FVector Target(75, 75, 75);
    const TSharedPtr<FJsonObject> Payload = BatchXformPayload({
        BatchXformEntry(Missing, TEXT("location"), FVector(1, 2, 3)),
        BatchXformEntry(A->GetActorLabel(), TEXT("location"), Target) });

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), Payload, Capture);
    TestTrue(TEXT("a partial batch is a success"), Capture.bSuccess);
    TestTrue(TEXT("the known actor still moved"), A->GetActorLocation().Equals(Target, 0.01));
    if (!TestTrue(TEXT("response carries a body"), Capture.Result.IsValid()))
    {
        return false;
    }
    TestFalse(TEXT("success flag is false when an entry failed"), Capture.Result->GetBoolField(TEXT("success")));
    TestEqual(TEXT("failedCount"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("failedCount"))), 1);
    const TSharedPtr<FJsonObject> Row0 = BatchXformRow(Capture.Result, 0);
    if (TestNotNull(TEXT("results[0] present"), Row0.Get()))
    {
        TestFalse(TEXT("results[0] failed"), Row0->GetBoolField(TEXT("success")));
        TestEqual(TEXT("results[0] carries ACTOR_NOT_FOUND"), Row0->GetStringField(TEXT("errorCode")), FString(TEXT("ACTOR_NOT_FOUND")));
    }
    const TArray<TSharedPtr<FJsonValue>>* MissingArr = nullptr;
    TestTrue(TEXT("missing[] lists the unknown name"),
        Capture.Result->TryGetArrayField(TEXT("missing"), MissingArr) && MissingArr && MissingArr->Num() == 1
        && (*MissingArr)[0]->AsString() == Missing);
    return true;
}

// Nothing resolvable: the batch is an error (never a zero-item success) and still reports per entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformBatchAllUnknownTest,
    "PinWright.actor.set_transform.BatchAllUnknownIsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformBatchAllUnknownTest::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const TSharedPtr<FJsonObject> Payload = BatchXformPayload({
        BatchXformEntry(TEXT("PW_BatchXformNone1_") + Guid, TEXT("location"), FVector::ZeroVector),
        BatchXformEntry(TEXT("PW_BatchXformNone2_") + Guid, TEXT("location"), FVector::ZeroVector) });

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), Payload, Capture);
    TestFalse(TEXT("an all-unknown batch is not a success"), Capture.bSuccess);
    TestEqual(TEXT("error code is ACTOR_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    if (TestTrue(TEXT("the error carries the per-entry body"), Capture.Result.IsValid()))
    {
        TestEqual(TEXT("failedCount"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("failedCount"))), 2);
    }
    return true;
}

// A malformed batch refuses before anything moves: a bad entry, an empty array, and actorName
// beside actors[] all return INVALID_ARGUMENT with the valid entry's actor untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformBatchMalformedTest,
    "PinWright.actor.set_transform.BatchMalformedEntryMovesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformBatchMalformedTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the malformed-batch assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    const FVector Start(10, 20, 30);
    AStaticMeshActor* A = BatchXformSpawnProbe(World, TEXT("PW_BatchXformMalformed"), Start);
    if (!TestNotNull(TEXT("probe spawned"), A))
    {
        return false;
    }

    // Entry 1 combines lookAt with rotation, which the verb refuses.
    TSharedPtr<FJsonObject> Bad = BatchXformEntry(A->GetActorLabel(), TEXT("lookAt"), FVector(1000, 0, 0));
    TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
    Rot->SetNumberField(TEXT("yaw"), 90.0);
    Bad->SetObjectField(TEXT("rotation"), Rot);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("actor.set_transform"),
        BatchXformPayload({ BatchXformEntry(A->GetActorLabel(), TEXT("location"), FVector(999, 999, 999)), Bad }), Capture);
    TestFalse(TEXT("a malformed entry refuses the batch"), Capture.bSuccess);
    TestEqual(TEXT("malformed entry yields INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the valid entry's actor did not move"), A->GetActorLocation().Equals(Start, 0.01));

    Capture.Reset();
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), BatchXformPayload({}), Capture);
    TestEqual(TEXT("empty actors[] yields INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    Capture.Reset();
    TSharedPtr<FJsonObject> Both = BatchXformPayload({ BatchXformEntry(A->GetActorLabel(), TEXT("location"), FVector(5, 5, 5)) });
    Both->SetStringField(TEXT("actorName"), A->GetActorLabel());
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), Both, Capture);
    TestEqual(TEXT("actorName beside actors[] yields INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("still unmoved after the refused calls"), A->GetActorLocation().Equals(Start, 0.01));
    return true;
}

// Single-form success keeps its pre-batch response: actorName/location/scale, no batch fields.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformSingleFormShapeTest,
    "PinWright.actor.set_transform.SingleFormResponseUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformSingleFormShapeTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the single-form shape assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* A = BatchXformSpawnProbe(World, TEXT("PW_BatchXformSingle"), FVector::ZeroVector);
    if (!TestNotNull(TEXT("probe spawned"), A))
    {
        return false;
    }
    const FVector Target(40, 50, 60);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("actor.set_transform"),
        BatchXformEntry(A->GetActorLabel(), TEXT("location"), Target), Capture);
    TestTrue(TEXT("single-form set_transform succeeds"), Capture.bSuccess);
    TestTrue(TEXT("actor moved"), A->GetActorLocation().Equals(Target, 0.01));
    if (TestTrue(TEXT("response carries a body"), Capture.Result.IsValid()))
    {
        TestEqual(TEXT("actorName is the label"), Capture.Result->GetStringField(TEXT("actorName")), A->GetActorLabel());
        TestTrue(TEXT("location is echoed"), Capture.Result->HasField(TEXT("location")));
        TestTrue(TEXT("scale is echoed"), Capture.Result->HasField(TEXT("scale")));
        TestFalse(TEXT("no batch results[] on the single form"), Capture.Result->HasField(TEXT("results")));
        TestFalse(TEXT("no lookAt block without lookAt"), Capture.Result->HasField(TEXT("lookAt")));
    }
    return true;
}

// lookAt a point: the measured forward axis points at the target from the NEW location, and roll
// is the twist about that axis.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformLookAtPointTest,
    "PinWright.actor.set_transform.LookAtPoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformLookAtPointTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the lookAt point assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* A = BatchXformSpawnProbe(World, TEXT("PW_BatchXformAimPoint"), FVector(-300, -300, 0));
    if (!TestNotNull(TEXT("probe spawned"), A))
    {
        return false;
    }
    const FVector NewLocation(1000, 0, 0);
    const FVector Target(1000, 500, 500);
    TSharedPtr<FJsonObject> Payload = BatchXformEntry(A->GetActorLabel(), TEXT("location"), NewLocation);
    Payload->SetObjectField(TEXT("lookAt"), BatchXformVec(Target));
    Payload->SetNumberField(TEXT("roll"), 30.0);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), Payload, Capture);
    TestTrue(TEXT("lookAt point succeeds"), Capture.bSuccess);
    const FVector Expected = (Target - NewLocation).GetSafeNormal();
    TestTrue(TEXT("forward axis points at the target from the new location"),
        FVector::DotProduct(A->GetActorForwardVector(), Expected) > FMath::Cos(FMath::DegreesToRadians(0.1)));
    TestTrue(TEXT("roll is applied about the aim axis"), FMath::IsNearlyEqual(A->GetActorRotation().Roll, 30.0, 0.01));

    const TSharedPtr<FJsonObject>* Aim = nullptr;
    if (TestTrue(TEXT("response carries a lookAt block"),
            Capture.Result.IsValid() && Capture.Result->TryGetObjectField(TEXT("lookAt"), Aim) && Aim))
    {
        TestTrue(TEXT("measured aimErrorDegrees is within tolerance"), (*Aim)->GetNumberField(TEXT("aimErrorDegrees")) <= 0.1);
        TestFalse(TEXT("a point target names no actor"), (*Aim)->HasField(TEXT("targetActor")));
    }
    return true;
}

// lookAt an actor (in batch form): aims at that actor's location, names it in the response.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformLookAtActorTest,
    "PinWright.actor.set_transform.LookAtActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformLookAtActorTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the lookAt actor assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* Camera = BatchXformSpawnProbe(World, TEXT("PW_BatchXformAimer"), FVector(0, 0, 0));
    AStaticMeshActor* Target = BatchXformSpawnProbe(World, TEXT("PW_BatchXformAimTarget"), FVector(300, 400, 200));
    if (!TestNotNull(TEXT("aimer spawned"), Camera) || !TestNotNull(TEXT("target spawned"), Target))
    {
        return false;
    }
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("actorName"), Camera->GetActorLabel());
    Entry->SetStringField(TEXT("lookAt"), Target->GetName());

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), BatchXformPayload({ Entry }), Capture);
    TestTrue(TEXT("lookAt actor succeeds"), Capture.bSuccess);
    const FVector Expected = (Target->GetActorLocation() - Camera->GetActorLocation()).GetSafeNormal();
    TestTrue(TEXT("forward axis points at the target actor"),
        FVector::DotProduct(Camera->GetActorForwardVector(), Expected) > FMath::Cos(FMath::DegreesToRadians(0.1)));
    TestTrue(TEXT("the target actor did not move"), Target->GetActorLocation().Equals(FVector(300, 400, 200), 0.01));

    const TSharedPtr<FJsonObject> Row0 = BatchXformRow(Capture.Result, 0);
    const TSharedPtr<FJsonObject>* Aim = nullptr;
    if (TestTrue(TEXT("results[0] carries a lookAt block"), Row0.IsValid() && Row0->TryGetObjectField(TEXT("lookAt"), Aim) && Aim))
    {
        TestEqual(TEXT("lookAt names the target by internal object name"),
            (*Aim)->GetStringField(TEXT("targetActorObjectName")), Target->GetName());
    }
    return true;
}

// lookAt refusals leave the actor untouched: with rotation, roll without lookAt, aiming at itself,
// and an unknown target actor (typed ACTOR_NOT_FOUND).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformLookAtRefusalsTest,
    "PinWright.actor.set_transform.LookAtConflictsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformLookAtRefusalsTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the lookAt refusal assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* A = BatchXformSpawnProbe(World, TEXT("PW_BatchXformAimRefuse"), FVector(0, 0, 0));
    if (!TestNotNull(TEXT("probe spawned"), A))
    {
        return false;
    }
    const FString Label = A->GetActorLabel();
    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> WithRotation = BatchXformEntry(Label, TEXT("lookAt"), FVector(100, 0, 0));
    TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
    Rot->SetNumberField(TEXT("yaw"), 45.0);
    WithRotation->SetObjectField(TEXT("rotation"), Rot);
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), WithRotation, Capture);
    TestEqual(TEXT("lookAt + rotation yields INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    Capture.Reset();
    TSharedPtr<FJsonObject> LoneRoll = MakeShared<FJsonObject>();
    LoneRoll->SetStringField(TEXT("actorName"), Label);
    LoneRoll->SetNumberField(TEXT("roll"), 10.0);
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), LoneRoll, Capture);
    TestEqual(TEXT("roll without lookAt yields INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    Capture.Reset();
    TSharedPtr<FJsonObject> Self = MakeShared<FJsonObject>();
    Self->SetStringField(TEXT("actorName"), Label);
    Self->SetStringField(TEXT("lookAt"), A->GetName());
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), Self, Capture);
    TestEqual(TEXT("aiming at itself yields INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    Capture.Reset();
    TSharedPtr<FJsonObject> Unknown = MakeShared<FJsonObject>();
    Unknown->SetStringField(TEXT("actorName"), Label);
    Unknown->SetStringField(TEXT("lookAt"), FString::Printf(TEXT("PW_BatchXformNoTarget_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    InvokeHandlerWithCapture(TEXT("actor.set_transform"), Unknown, Capture);
    TestEqual(TEXT("unknown lookAt target yields ACTOR_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));

    TestTrue(TEXT("no refused call rotated the actor"), A->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
    return true;
}

// Batch nudge: each entry's delta is added to its own actor, under one call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorNudgeBatchTest,
    "PinWright.actor.nudge.BatchNudgesEveryEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorNudgeBatchTest::RunTest(const FString& Parameters)
{
    UWorld* World = BatchXformEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping the batch nudge assertions."));
        return true;
    }
    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* A = BatchXformSpawnProbe(World, TEXT("PW_BatchNudgeA"), FVector(0, 0, 0));
    AStaticMeshActor* B = BatchXformSpawnProbe(World, TEXT("PW_BatchNudgeB"), FVector(200, 0, 0));
    if (!TestNotNull(TEXT("probe A spawned"), A) || !TestNotNull(TEXT("probe B spawned"), B))
    {
        return false;
    }
    TSharedPtr<FJsonObject> RotB = MakeShared<FJsonObject>();
    RotB->SetNumberField(TEXT("pitch"), 0.0);
    RotB->SetNumberField(TEXT("yaw"), 45.0);
    RotB->SetNumberField(TEXT("roll"), 0.0);
    TSharedPtr<FJsonObject> EntryB = BatchXformEntry(B->GetActorLabel(), TEXT("deltaWorld"), FVector(0, 0, 50));
    EntryB->SetObjectField(TEXT("deltaRotation"), RotB);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.nudge is registered"), InvokeHandlerWithCapture(TEXT("actor.nudge"),
        BatchXformPayload({ BatchXformEntry(A->GetActorLabel(), TEXT("deltaWorld"), FVector(10, 20, 30)), EntryB }), Capture));
    TestTrue(TEXT("batch nudge succeeds"), Capture.bSuccess);
    TestTrue(TEXT("A moved by its own delta"), A->GetActorLocation().Equals(FVector(10, 20, 30), 0.01));
    TestTrue(TEXT("B moved by its own delta"), B->GetActorLocation().Equals(FVector(200, 0, 50), 0.01));
    TestTrue(TEXT("B rotated by its own delta"), FMath::IsNearlyEqual(B->GetActorRotation().Yaw, 45.0, 0.01));
    TestTrue(TEXT("A did not rotate"), A->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));

    const TSharedPtr<FJsonObject> Row0 = BatchXformRow(Capture.Result, 0);
    if (TestNotNull(TEXT("results[0] present"), Row0.Get()))
    {
        TestTrue(TEXT("rows keep the single-form appliedDelta"), Row0->HasField(TEXT("appliedDelta")));
    }

    // An entry with no delta is a malformed batch: refused, nothing moves.
    Capture.Reset();
    TSharedPtr<FJsonObject> NoDelta = MakeShared<FJsonObject>();
    NoDelta->SetStringField(TEXT("actorName"), B->GetActorLabel());
    InvokeHandlerWithCapture(TEXT("actor.nudge"),
        BatchXformPayload({ BatchXformEntry(A->GetActorLabel(), TEXT("deltaWorld"), FVector(1, 1, 1)), NoDelta }), Capture);
    TestEqual(TEXT("an entry with no delta yields INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("A unmoved by the refused batch"), A->GetActorLocation().Equals(FVector(10, 20, 30), 0.01));
    return true;
}
