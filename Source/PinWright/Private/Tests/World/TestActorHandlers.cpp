// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Actor domain handlers.
// Covers all 30 handlers across SpawnHandler, LifecycleHandler, QueryHandler,
// ActorPropertyHandler, ActorTransformHandler, and ComponentHandler.
// Each test verifies that the handler is registered and does not crash when
// invoked with missing required params (expects early-exit error path) or with
// realistic fake param values (expects handler to reach the actor-lookup path).
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"
#include "Engine/Level.h"
#include "LevelUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    const FHandlerRegistration* FindRegistration(const FString& MethodName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == MethodName)
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    bool HasParam(const FHandlerRegistration* Reg, const TCHAR* ParamName)
    {
        if (!Reg)
        {
            return false;
        }

        for (const FParamSpec& Param : Reg->Params)
        {
            if (Param.Name == ParamName)
            {
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// SpawnHandler — actor.spawn
// ============================================================================

// actor.spawn has no RPC_PARAM_REQ, but its body requires either classPath or
// meshPath at runtime.  Calling with an empty payload triggers the runtime
// INVALID_PARAMS error path and must not crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnMissingClassAndMeshTest,
    "PinWright.actor.spawn.MissingClassAndMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnMissingClassAndMeshTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.spawn handler is registered"), InvokeHandler(TEXT("actor.spawn"), Payload));
    return true;
}

// Provide a classPath so the handler proceeds past the early validation check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnWithClassPathTest,
    "PinWright.actor.spawn.WithClassPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnWithClassPathTest::RunTest(const FString& Parameters)
{
    // Spawns a real PointLight into the editor world (L_Core); the guard destroys
    // it and restores the level's dirty flag so the test leaves the map untouched.
    FScopedEditorWorldActorGuard WorldGuard;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
    Payload->SetStringField(TEXT("actorName"), TEXT("TestPointLight"));
    TestTrue(TEXT("actor.spawn handler is registered"), InvokeHandler(TEXT("actor.spawn"), Payload));
    return true;
}

// Regression test for E-spawn-no-scale-param: actor.spawn must accept an optional
// `scale` {x,y,z} param and APPLY it to the spawned actor's transform, so a
// non-unit prop lands scaled in one call instead of forcing a follow-up
// actor.set_transform. Spawns a real PointLight with a non-unit scale through the
// production handler, then reads the actual actor's world scale back from the
// editor world and asserts it matches the request. If the scale param / its
// transform application were reverted, the spawned actor would land at unit (1,1,1)
// and the world-scale assertion below would fail. Also asserts the param is
// discoverable on both spawn verbs' registered schema (the ticket's discoverability
// angle).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnAppliesScaleTest,
    "PinWright.actor.spawn.AppliesScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnAppliesScaleTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.spawn scale test."));
        return true;
    }

    // The `scale` slot must be registered on both spawn verbs so callers can discover it.
    const FHandlerRegistration* SpawnReg = FindRegistration(TEXT("actor.spawn"));
    TestNotNull(TEXT("actor.spawn registration exists"), SpawnReg);
    TestTrue(TEXT("actor.spawn registers a scale param"), HasParam(SpawnReg, TEXT("scale")));
    TestTrue(TEXT("actor.spawn_from_blueprint registers a scale param"),
        HasParam(FindRegistration(TEXT("actor.spawn_from_blueprint")), TEXT("scale")));

    // Spawn a real PointLight with a non-unit scale; the guard destroys it and
    // restores the level's dirty flag on scope exit.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString UniqueLabel = FString::Printf(TEXT("SpawnScaleProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FVector RequestedScale(2.0, 3.0, 0.5);

    TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
    ScaleObj->SetNumberField(TEXT("x"), RequestedScale.X);
    ScaleObj->SetNumberField(TEXT("y"), RequestedScale.Y);
    ScaleObj->SetNumberField(TEXT("z"), RequestedScale.Z);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
    Payload->SetStringField(TEXT("actorName"), UniqueLabel);
    Payload->SetObjectField(TEXT("scale"), ScaleObj);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
    TestTrue(TEXT("actor.spawn with a scale succeeds"), Capture.bSuccess);

    // Find the spawned actor and assert its WORLD scale equals the request — proves
    // the handler applied the scale at spawn, not merely that it accepted the param.
    // Resolve via the production label resolver (the GUID-suffixed label is unambiguous),
    // independent of the response under test.
    AActor* Spawned = McpActorUtils::FindActorByName(nullptr, UniqueLabel);
    TestNotNull(TEXT("spawned actor found in the editor world"), Spawned);
    if (Spawned)
    {
        TestTrue(TEXT("spawned actor carries the requested non-unit scale (not unit (1,1,1))"),
            Spawned->GetActorScale3D().Equals(RequestedScale, 0.01));
    }
    return true;
}

// ============================================================================
// SpawnHandler — actor.spawn_from_blueprint
// ============================================================================

// blueprintPath is RPC_PARAM_REQ — omitting it triggers the INVALID_ARGUMENT path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnFromBlueprintMissingBlueprintPathTest,
    "PinWright.actor.spawn_from_blueprint.MissingBlueprintPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnFromBlueprintMissingBlueprintPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.spawn_from_blueprint handler is registered"),
        InvokeHandler(TEXT("actor.spawn_from_blueprint"), Payload));
    return true;
}

// Provide blueprintPath so the handler passes validation and reaches asset-load logic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnFromBlueprintWithPathTest,
    "PinWright.actor.spawn_from_blueprint.WithPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnFromBlueprintWithPathTest::RunTest(const FString& Parameters)
{
    // Cleans up any actor spawned into the editor world (L_Core); the test
    // blueprint path does not exist, so usually nothing spawns, but the guard
    // keeps the map clean if it ever does.
    FScopedEditorWorldActorGuard WorldGuard;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/BP_TestActor"));
    Payload->SetStringField(TEXT("actorName"), TEXT("SpawnedBP"));
    bSuppressLogErrors = true;
    TestTrue(TEXT("actor.spawn_from_blueprint handler is registered"),
        InvokeHandler(TEXT("actor.spawn_from_blueprint"), Payload));
    return true;
}

// ============================================================================
// LifecycleHandler — actor.delete
// ============================================================================

// actor.delete has only optional params but validates at runtime — empty payload
// produces INVALID_ARGUMENT.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDeleteMissingNamesTest,
    "PinWright.actor.delete.MissingNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDeleteMissingNamesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.delete handler is registered"), InvokeHandler(TEXT("actor.delete"), Payload));
    return true;
}

// Provide actorName — handler proceeds to the actor-lookup path (NOT_FOUND expected).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDeleteValidParamsTest,
    "PinWright.actor.delete.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDeleteValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    TestTrue(TEXT("actor.delete handler is registered"), InvokeHandler(TEXT("actor.delete"), Payload));
    return true;
}

// ============================================================================
// LifecycleHandler — actor.duplicate
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateMissingActorNameTest,
    "PinWright.actor.duplicate.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDuplicateMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.duplicate handler is registered"), InvokeHandler(TEXT("actor.duplicate"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateValidParamsTest,
    "PinWright.actor.duplicate.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDuplicateValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("newName"), TEXT("NonExistentActor_Copy"));
    TestTrue(TEXT("actor.duplicate handler is registered"), InvokeHandler(TEXT("actor.duplicate"), Payload));
    return true;
}

// Regression test for E-actor-duplicate-locked-level-opaque-error: duplicating
// an actor whose level is LOCKED must surface a diagnostic LEVEL_LOCKED error
// that names the cause and the level.set_locked recovery, not the bare opaque
// DUPLICATE_FAILED. Spawns a real actor into the editor world, locks its level,
// invokes the production actor.duplicate handler, and asserts the LEVEL_LOCKED
// code; then unlocks and confirms the identical call succeeds (proving the lock
// is the sole variable). If the pre-check were reverted, the locked-level call
// would fall through to the engine's null-result path and report DUPLICATE_FAILED
// instead, failing the code assertion below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateLockedLevelDiagnosticTest,
    "PinWright.actor.duplicate.LockedLevelDiagnostic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDuplicateLockedLevelDiagnosticTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world/persistent level available; skipping locked-level duplicate test."));
        return true;
    }

    // Spawn a real source actor; the guard destroys it and restores the dirty flag.
    FScopedEditorWorldActorGuard WorldGuard;
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"), TEXT("DuplicateLockSource"));
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        TestTrue(TEXT("actor.spawn succeeded"), SpawnCapture.bSuccess);
    }

    ULevel* SourceLevel = World->PersistentLevel;

    // Build the duplicate payload once so the locked and unlocked invocations are
    // literally the same call — the lock state is then the only variable between them.
    auto MakeDuplicatePayload = []()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TEXT("DuplicateLockSource"));
        TSharedPtr<FJsonObject> OffsetObj = MakeShared<FJsonObject>();
        OffsetObj->SetNumberField(TEXT("x"), 300.0);
        Payload->SetObjectField(TEXT("offset"), OffsetObj);
        return Payload;
    };

    // RAII guard locks the level now and restores its original state on every exit
    // path (including an aborting check macro), so a leaked lock can't pollute later tests.
    FScopedLevelLock LevelLock(SourceLevel, /*bDesiredLocked=*/true);
    TestTrue(TEXT("Source level is locked for the test"), FLevelUtils::IsLevelLocked(SourceLevel));

    // Locked: the handler must report the diagnostic LEVEL_LOCKED code, not DUPLICATE_FAILED.
    {
        FTestResponseCapture Capture;
        bSuppressLogErrors = true;
        TestTrue(TEXT("actor.duplicate handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.duplicate"), MakeDuplicatePayload(), Capture));
        TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
        TestFalse(TEXT("Duplicate on a locked level is an error"), Capture.bSuccess);
        TestEqual(TEXT("Error code names the locked-level cause (LEVEL_LOCKED, not DUPLICATE_FAILED)"),
            Capture.ErrorCode, FString(TEXT("LEVEL_LOCKED")));
    }

    // Unlock and confirm the identical call now succeeds — lock was the sole variable.
    LevelLock.SetLocked(false);
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("actor.duplicate handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.duplicate"), MakeDuplicatePayload(), Capture));
        TestTrue(TEXT("Identical duplicate succeeds once the level is unlocked"), Capture.bSuccess);
    }

    return true;
}

// ============================================================================
// LifecycleHandler — actor.delete_by_tag
// ============================================================================

// tag is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDeleteByTagMissingTagTest,
    "PinWright.actor.delete_by_tag.MissingTag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDeleteByTagMissingTagTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.delete_by_tag handler is registered"),
        InvokeHandler(TEXT("actor.delete_by_tag"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDeleteByTagValidParamsTest,
    "PinWright.actor.delete_by_tag.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDeleteByTagValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tag"), TEXT("Destructible"));
    TestTrue(TEXT("actor.delete_by_tag handler is registered"),
        InvokeHandler(TEXT("actor.delete_by_tag"), Payload));
    return true;
}

// ============================================================================
// LifecycleHandler — actor.export
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorExportMissingActorNameTest,
    "PinWright.actor.export.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorExportMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.export handler is registered"), InvokeHandler(TEXT("actor.export"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorExportValidParamsTest,
    "PinWright.actor.export.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorExportValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    TestTrue(TEXT("actor.export handler is registered"), InvokeHandler(TEXT("actor.export"), Payload));
    return true;
}

// ============================================================================
// QueryHandler — actor.list
// ============================================================================

// actor.list is all-optional — empty payload is valid and must not crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListNoCrashTest,
    "PinWright.actor.list.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filter"), TEXT("Light"));
    TestTrue(TEXT("actor.list handler is registered"), InvokeHandler(TEXT("actor.list"), Payload));
    return true;
}

// Regression test for E-actor-list-no-limit-spills: actor.list must offer a
// `limit` cap (with an untruncated `totalMatches` + `truncated` flag) and a
// `namesOnly`/`fields` projection that drops the verbose per-row `path`, so the
// natural "enumerate so I can pick a few" call can stay inline instead of always
// spilling the full array to disk. Spawns three real PointLights into the editor
// world, filters to them by a unique label fragment so the assertions are
// independent of whatever else the open map contains, then drives the production
// handler. If the limit/projection plumbing were reverted, limit would be ignored
// (count would equal totalMatches with truncated:false) and the namesOnly call would
// still carry `path`, failing the assertions below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListLimitAndProjectionTest,
    "PinWright.actor.list.LimitAndProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListLimitAndProjectionTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.list limit/projection test."));
        return true;
    }

    // Spawn three real PointLights sharing a unique label fragment; the guard
    // destroys them and restores the level's dirty flag on scope exit.
    FScopedEditorWorldActorGuard WorldGuard;
    const FString LabelTag = TEXT("ActorListLimitProbe");
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"),
            FString::Printf(TEXT("%s_%d"), *LabelTag, Index));
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        TestTrue(TEXT("actor.spawn succeeded"), SpawnCapture.bSuccess);
    }

    // Each probe seeds the shared filter+world narrowing onto the three spawned
    // lights, lets the caller add the case-specific knob, invokes the production
    // handler, and asserts it registered + succeeded — so each case below is just
    // its distinguishing payload and its own assertions.
    auto ListProbe = [&](TFunctionRef<void(TSharedPtr<FJsonObject>)> Configure) -> FTestResponseCapture
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filter"), LabelTag);
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Configure(Payload);
        TestTrue(TEXT("actor.list handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.list"), Payload, Capture));
        TestTrue(TEXT("actor.list succeeded"), Capture.bSuccess);
        return Capture;
    };

    // Digs the first row object out of a list response, or nullptr if absent.
    auto FirstRow = [&](FTestResponseCapture& Capture) -> const TSharedPtr<FJsonObject>*
    {
        const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
        if (!Capture.Result.IsValid()
            || !Capture.Result->TryGetArrayField(TEXT("actors"), ActorsArr)
            || !ActorsArr || ActorsArr->Num() == 0)
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if ((*ActorsArr)[0].IsValid() && (*ActorsArr)[0]->TryGetObject(Row) && Row)
        {
            return Row;
        }
        return nullptr;
    };

    // Baseline: filter to just the three probes, no limit. totalMatches equals the
    // returned count and truncated is false.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject>) {});
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            double Count = 0.0, Total = 0.0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), Total);
            bool bTruncated = true;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestEqual(TEXT("Unlimited list returns all 3 probes"), (int32)Count, 3);
            TestEqual(TEXT("totalMatches matches the returned count when uncapped"),
                (int32)Total, 3);
            TestFalse(TEXT("truncated is false when nothing was capped"), bTruncated);
        }
    }

    // limit=2: count caps at 2 rows, but totalMatches still reports all 3 matches
    // and truncated flips true so the caller knows the list was elided.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject> Payload)
        {
            Payload->SetNumberField(TEXT("limit"), 2);
        });
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            double Count = 0.0, Total = 0.0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), Total);
            bool bTruncated = false;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestEqual(TEXT("limit=2 caps the returned rows at 2"), (int32)Count, 2);
            TestEqual(TEXT("totalMatches still reports all 3 untruncated matches"),
                (int32)Total, 3);
            TestTrue(TEXT("truncated is true when limit elided rows"), bTruncated);

            const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("actors"), ActorsArr) && ActorsArr)
            {
                TestEqual(TEXT("actors array length honors the limit"), ActorsArr->Num(), 2);
            }
        }
    }

    // namesOnly=true: each returned actor row carries label/name/class but NOT the
    // verbose path field — this is the projection that collapses the payload.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject> Payload)
        {
            Payload->SetBoolField(TEXT("namesOnly"), true);
        });
        if (const TSharedPtr<FJsonObject>* Row = FirstRow(Capture))
        {
            TestTrue(TEXT("namesOnly row keeps label"), (*Row)->HasField(TEXT("label")));
            TestTrue(TEXT("namesOnly row keeps class"), (*Row)->HasField(TEXT("class")));
            TestFalse(TEXT("namesOnly row drops the verbose path field"),
                (*Row)->HasField(TEXT("path")));
        }
    }

    // fields=["label"] allow-list: rows carry only label, dropping name/path/class.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject> Payload)
        {
            TArray<TSharedPtr<FJsonValue>> FieldsArr;
            FieldsArr.Add(MakeShared<FJsonValueString>(TEXT("label")));
            Payload->SetArrayField(TEXT("fields"), FieldsArr);
        });
        if (const TSharedPtr<FJsonObject>* Row = FirstRow(Capture))
        {
            TestTrue(TEXT("fields=[label] row keeps label"), (*Row)->HasField(TEXT("label")));
            TestFalse(TEXT("fields=[label] row drops name"), (*Row)->HasField(TEXT("name")));
            TestFalse(TEXT("fields=[label] row drops path"), (*Row)->HasField(TEXT("path")));
            TestFalse(TEXT("fields=[label] row drops class"), (*Row)->HasField(TEXT("class")));
        }
    }

    return true;
}

// ============================================================================
// QueryHandler — actor.get
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetMissingActorNameTest,
    "PinWright.actor.get.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.get handler is registered"), InvokeHandler(TEXT("actor.get"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetValidParamsTest,
    "PinWright.actor.get.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    TestTrue(TEXT("actor.get handler is registered"), InvokeHandler(TEXT("actor.get"), Payload));
    return true;
}

// ============================================================================
// DescribeHandler — actor.describe
// ============================================================================

// actorName is RPC_PARAM_REQ, with objectPath accepted as an alias.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDescribeMissingActorNameTest,
    "PinWright.actor.describe.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDescribeMissingActorNameTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.describe handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.describe"), Payload, Capture));
    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (missing actorName)"), Capture.bSuccess);
    TestEqual(TEXT("Error code is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDescribeMissingActorTest,
    "PinWright.actor.describe.MissingActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDescribeMissingActorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    bSuppressLogErrors = true;
    TestTrue(TEXT("actor.describe handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.describe"), Payload, Capture));
    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (missing actor)"), Capture.bSuccess);
    TestEqual(TEXT("Error code is ACTOR_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDescribeObjectPathAliasMissingActorTest,
    "PinWright.actor.describe.ObjectPathAliasMissingActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDescribeObjectPathAliasMissingActorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("NonExistentActor"));
    bSuppressLogErrors = true;
    TestTrue(TEXT("actor.describe handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.describe"), Payload, Capture));
    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (missing actor)"), Capture.bSuccess);
    TestEqual(TEXT("Error code is ACTOR_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorReadComponentFilterParamsRegisteredTest,
    "PinWright.actor.ComponentReadFilterParamsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorReadComponentFilterParamsRegisteredTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* DescribeReg = FindRegistration(TEXT("actor.describe"));
    TestNotNull(TEXT("actor.describe registration exists"), DescribeReg);
    TestTrue(TEXT("actor.describe registers nameMatch"),
        HasParam(DescribeReg, TEXT("nameMatch")));
    TestTrue(TEXT("actor.describe registers name_match alias"),
        HasParam(DescribeReg, TEXT("name_match")));
    TestTrue(TEXT("actor.describe registers componentClass"),
        HasParam(DescribeReg, TEXT("componentClass")));
    TestTrue(TEXT("actor.describe registers component_class alias"),
        HasParam(DescribeReg, TEXT("component_class")));

    const FHandlerRegistration* GetComponentsReg = FindRegistration(TEXT("actor.get_components"));
    TestNotNull(TEXT("actor.get_components registration exists"), GetComponentsReg);
    TestTrue(TEXT("actor.get_components registers nameMatch"),
        HasParam(GetComponentsReg, TEXT("nameMatch")));
    TestTrue(TEXT("actor.get_components registers name_match alias"),
        HasParam(GetComponentsReg, TEXT("name_match")));
    TestTrue(TEXT("actor.get_components registers componentClass"),
        HasParam(GetComponentsReg, TEXT("componentClass")));
    TestTrue(TEXT("actor.get_components registers component_class alias"),
        HasParam(GetComponentsReg, TEXT("component_class")));
    return true;
}

// ============================================================================
// QueryHandler — actor.find_by_name
// ============================================================================

// name is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByNameMissingNameTest,
    "PinWright.actor.find_by_name.MissingName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByNameMissingNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.find_by_name handler is registered"),
        InvokeHandler(TEXT("actor.find_by_name"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByNameValidParamsTest,
    "PinWright.actor.find_by_name.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByNameValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("PointLight"));
    TestTrue(TEXT("actor.find_by_name handler is registered"),
        InvokeHandler(TEXT("actor.find_by_name"), Payload));
    return true;
}

// ============================================================================
// QueryHandler — actor.find_by_tag
// ============================================================================

// tag is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByTagMissingTagTest,
    "PinWright.actor.find_by_tag.MissingTag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByTagMissingTagTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.find_by_tag handler is registered"),
        InvokeHandler(TEXT("actor.find_by_tag"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByTagValidParamsTest,
    "PinWright.actor.find_by_tag.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByTagValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tag"), TEXT("Enemy"));
    Payload->SetStringField(TEXT("matchType"), TEXT("exact"));
    TestTrue(TEXT("actor.find_by_tag handler is registered"),
        InvokeHandler(TEXT("actor.find_by_tag"), Payload));
    return true;
}

// ============================================================================
// QueryHandler — actor.find_by_class
// ============================================================================

// className is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByClassMissingClassNameTest,
    "PinWright.actor.find_by_class.MissingClassName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByClassMissingClassNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.find_by_class handler is registered"),
        InvokeHandler(TEXT("actor.find_by_class"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByClassValidParamsTest,
    "PinWright.actor.find_by_class.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByClassValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("className"), TEXT("PointLight"));
    TestTrue(TEXT("actor.find_by_class handler is registered"),
        InvokeHandler(TEXT("actor.find_by_class"), Payload));
    return true;
}

// Regression test for B-actor-find-by-class-short-name-fails: a short class name
// must resolve and find the actor that was spawned, instead of silently returning
// count:0. Spawns a real PointLight into the editor world, then queries by the
// short name 'PointLight' (the literal documented form) and asserts the spawned
// actor is found. If the handler reverts to raw FindObject<UClass> on the short
// name (which resolves to null), this test fails: count would drop to 0 and the
// success/count assertions below would not hold.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByClassShortNameResolvesTest,
    "PinWright.actor.find_by_class.ShortNameResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByClassShortNameResolvesTest::RunTest(const FString& Parameters)
{
    // Spawn a real PointLight into the editor world and clean it up afterwards.
    FScopedEditorWorldActorGuard WorldGuard;
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"), TEXT("FindByClassRegressionLight"));
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        TestTrue(TEXT("actor.spawn succeeded"), SpawnCapture.bSuccess);
    }

    // Query by the SHORT class name 'PointLight' against the editor world.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("className"), TEXT("PointLight"));
    Payload->SetStringField(TEXT("world"), TEXT("editor"));
    TestTrue(TEXT("actor.find_by_class handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.find_by_class"), Payload, Capture));

    TestTrue(TEXT("Short class name resolves (no CLASS_NOT_FOUND)"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Count = 0.0;
        Capture.Result->TryGetNumberField(TEXT("count"), Count);
        // The spawned PointLight (plus any pre-existing ones) must be found.
        TestTrue(TEXT("Short class name 'PointLight' finds at least one actor (count >= 1)"),
            Count >= 1.0);
    }
    return true;
}

// Regression guard for the new CLASS_NOT_FOUND error path: an unresolvable class
// name must now surface an error rather than a silent success-with-count:0, so
// callers can distinguish "class not found" from "zero actors of that class".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByClassUnresolvableErrorsTest,
    "PinWright.actor.find_by_class.UnresolvableErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByClassUnresolvableErrorsTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("className"), TEXT("ThisClassDoesNotExist_ABC123"));
    bSuppressLogErrors = true;
    TestTrue(TEXT("actor.find_by_class handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.find_by_class"), Payload, Capture));

    TestFalse(TEXT("Unresolvable class name is an error, not a silent count:0"), Capture.bSuccess);
    TestEqual(TEXT("Error code is CLASS_NOT_FOUND"), Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.set_visibility
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetVisibilityMissingActorNameTest,
    "PinWright.actor.set_visibility.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetVisibilityMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.set_visibility handler is registered"),
        InvokeHandler(TEXT("actor.set_visibility"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetVisibilityValidParamsTest,
    "PinWright.actor.set_visibility.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetVisibilityValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetBoolField(TEXT("visible"), false);
    TestTrue(TEXT("actor.set_visibility handler is registered"),
        InvokeHandler(TEXT("actor.set_visibility"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.apply_force
// ============================================================================

// actorName and force are both RPC_PARAM_REQ — omitting both triggers early exit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorApplyForceMissingParamsTest,
    "PinWright.actor.apply_force.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorApplyForceMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.apply_force handler is registered"),
        InvokeHandler(TEXT("actor.apply_force"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorApplyForceValidParamsTest,
    "PinWright.actor.apply_force.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorApplyForceValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> ForceObj = MakeShared<FJsonObject>();
    ForceObj->SetNumberField(TEXT("x"), 500.0);
    ForceObj->SetNumberField(TEXT("y"), 0.0);
    ForceObj->SetNumberField(TEXT("z"), 0.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetObjectField(TEXT("force"), ForceObj);
    TestTrue(TEXT("actor.apply_force handler is registered"),
        InvokeHandler(TEXT("actor.apply_force"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.set_collision
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetCollisionMissingActorNameTest,
    "PinWright.actor.set_collision.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetCollisionMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.set_collision handler is registered"),
        InvokeHandler(TEXT("actor.set_collision"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetCollisionValidParamsTest,
    "PinWright.actor.set_collision.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetCollisionValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetBoolField(TEXT("collisionEnabled"), true);
    TestTrue(TEXT("actor.set_collision handler is registered"),
        InvokeHandler(TEXT("actor.set_collision"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.add_tag
// ============================================================================

// actorName and tag are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorAddTagMissingParamsTest,
    "PinWright.actor.add_tag.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorAddTagMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.add_tag handler is registered"), InvokeHandler(TEXT("actor.add_tag"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorAddTagValidParamsTest,
    "PinWright.actor.add_tag.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorAddTagValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("tag"), TEXT("Pickable"));
    TestTrue(TEXT("actor.add_tag handler is registered"), InvokeHandler(TEXT("actor.add_tag"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.remove_tag
// ============================================================================

// actorName and tag are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorRemoveTagMissingParamsTest,
    "PinWright.actor.remove_tag.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorRemoveTagMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.remove_tag handler is registered"),
        InvokeHandler(TEXT("actor.remove_tag"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorRemoveTagValidParamsTest,
    "PinWright.actor.remove_tag.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorRemoveTagValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("tag"), TEXT("Pickable"));
    TestTrue(TEXT("actor.remove_tag handler is registered"),
        InvokeHandler(TEXT("actor.remove_tag"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.set_blueprint_variables
// ============================================================================

// actorName and variables are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetBlueprintVariablesMissingParamsTest,
    "PinWright.actor.set_blueprint_variables.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetBlueprintVariablesMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.set_blueprint_variables handler is registered"),
        InvokeHandler(TEXT("actor.set_blueprint_variables"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetBlueprintVariablesValidParamsTest,
    "PinWright.actor.set_blueprint_variables.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetBlueprintVariablesValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
    Variables->SetNumberField(TEXT("Health"), 100.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetObjectField(TEXT("variables"), Variables);
    TestTrue(TEXT("actor.set_blueprint_variables handler is registered"),
        InvokeHandler(TEXT("actor.set_blueprint_variables"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.attach
// ============================================================================

// childActor and parentActor are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorAttachMissingParamsTest,
    "PinWright.actor.attach.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorAttachMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.attach handler is registered"), InvokeHandler(TEXT("actor.attach"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorAttachValidParamsTest,
    "PinWright.actor.attach.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorAttachValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("childActor"), TEXT("ChildActorName"));
    Payload->SetStringField(TEXT("parentActor"), TEXT("ParentActorName"));
    TestTrue(TEXT("actor.attach handler is registered"), InvokeHandler(TEXT("actor.attach"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.detach
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDetachMissingActorNameTest,
    "PinWright.actor.detach.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDetachMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.detach handler is registered"), InvokeHandler(TEXT("actor.detach"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDetachValidParamsTest,
    "PinWright.actor.detach.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDetachValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    TestTrue(TEXT("actor.detach handler is registered"), InvokeHandler(TEXT("actor.detach"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.create_snapshot
// ============================================================================

// actorName and snapshotName are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorCreateSnapshotMissingParamsTest,
    "PinWright.actor.create_snapshot.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorCreateSnapshotMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.create_snapshot handler is registered"),
        InvokeHandler(TEXT("actor.create_snapshot"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorCreateSnapshotValidParamsTest,
    "PinWright.actor.create_snapshot.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorCreateSnapshotValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("snapshotName"), TEXT("InitialPose"));
    TestTrue(TEXT("actor.create_snapshot handler is registered"),
        InvokeHandler(TEXT("actor.create_snapshot"), Payload));
    return true;
}

// ============================================================================
// ActorPropertyHandler — actor.restore_snapshot
// ============================================================================

// actorName and snapshotName are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorRestoreSnapshotMissingParamsTest,
    "PinWright.actor.restore_snapshot.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorRestoreSnapshotMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.restore_snapshot handler is registered"),
        InvokeHandler(TEXT("actor.restore_snapshot"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorRestoreSnapshotValidParamsTest,
    "PinWright.actor.restore_snapshot.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorRestoreSnapshotValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("snapshotName"), TEXT("InitialPose"));
    TestTrue(TEXT("actor.restore_snapshot handler is registered"),
        InvokeHandler(TEXT("actor.restore_snapshot"), Payload));
    return true;
}

// ============================================================================
// ActorTransformHandler — actor.set_transform
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformMissingActorNameTest,
    "PinWright.actor.set_transform.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.set_transform handler is registered"),
        InvokeHandler(TEXT("actor.set_transform"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetTransformValidParamsTest,
    "PinWright.actor.set_transform.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetTransformValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
    LocationObj->SetNumberField(TEXT("x"), 100.0);
    LocationObj->SetNumberField(TEXT("y"), 200.0);
    LocationObj->SetNumberField(TEXT("z"), 300.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetObjectField(TEXT("location"), LocationObj);
    TestTrue(TEXT("actor.set_transform handler is registered"),
        InvokeHandler(TEXT("actor.set_transform"), Payload));
    return true;
}

// ============================================================================
// ActorTransformHandler — actor.get_transform
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetTransformMissingActorNameTest,
    "PinWright.actor.get_transform.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetTransformMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.get_transform handler is registered"),
        InvokeHandler(TEXT("actor.get_transform"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetTransformValidParamsTest,
    "PinWright.actor.get_transform.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetTransformValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    TestTrue(TEXT("actor.get_transform handler is registered"),
        InvokeHandler(TEXT("actor.get_transform"), Payload));
    return true;
}

// ============================================================================
// ActorTransformHandler — actor.get_bounding_box
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetBoundingBoxMissingActorNameTest,
    "PinWright.actor.get_bounding_box.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetBoundingBoxMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.get_bounding_box handler is registered"),
        InvokeHandler(TEXT("actor.get_bounding_box"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetBoundingBoxValidParamsTest,
    "PinWright.actor.get_bounding_box.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetBoundingBoxValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    TestTrue(TEXT("actor.get_bounding_box handler is registered"),
        InvokeHandler(TEXT("actor.get_bounding_box"), Payload));
    return true;
}

// ============================================================================
// ComponentHandler — actor.add_component
// ============================================================================

// actorName and componentType are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorAddComponentMissingParamsTest,
    "PinWright.actor.add_component.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorAddComponentMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.add_component handler is registered"),
        InvokeHandler(TEXT("actor.add_component"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorAddComponentValidParamsTest,
    "PinWright.actor.add_component.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorAddComponentValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("componentType"), TEXT("StaticMeshComponent"));
    Payload->SetStringField(TEXT("componentName"), TEXT("MyMeshComp"));
    TestTrue(TEXT("actor.add_component handler is registered"),
        InvokeHandler(TEXT("actor.add_component"), Payload));
    return true;
}

// ============================================================================
// ComponentHandler — actor.set_component_properties
// ============================================================================

// actorName, componentName, and properties are all RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetComponentPropertiesMissingParamsTest,
    "PinWright.actor.set_component_properties.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetComponentPropertiesMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.set_component_properties handler is registered"),
        InvokeHandler(TEXT("actor.set_component_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetComponentPropertiesValidParamsTest,
    "PinWright.actor.set_component_properties.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetComponentPropertiesValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("Mobility"), TEXT("Movable"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("componentName"), TEXT("StaticMeshComponent0"));
    Payload->SetObjectField(TEXT("properties"), Properties);
    TestTrue(TEXT("actor.set_component_properties handler is registered"),
        InvokeHandler(TEXT("actor.set_component_properties"), Payload));
    return true;
}

// ============================================================================
// ComponentHandler — actor.get_components
// ============================================================================

// actorName is RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentsMissingActorNameTest,
    "PinWright.actor.get_components.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentsMissingActorNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.get_components handler is registered"),
        InvokeHandler(TEXT("actor.get_components"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentsValidParamsTest,
    "PinWright.actor.get_components.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    bSuppressLogErrors = true;
    TestTrue(TEXT("actor.get_components handler is registered"),
        InvokeHandler(TEXT("actor.get_components"), Payload));
    return true;
}

// ============================================================================
// ComponentHandler — actor.remove_component
// ============================================================================

// actorName and componentName are both RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorRemoveComponentMissingParamsTest,
    "PinWright.actor.remove_component.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorRemoveComponentMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.remove_component handler is registered"),
        InvokeHandler(TEXT("actor.remove_component"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorRemoveComponentValidParamsTest,
    "PinWright.actor.remove_component.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorRemoveComponentValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("componentName"), TEXT("StaticMeshComponent0"));
    TestTrue(TEXT("actor.remove_component handler is registered"),
        InvokeHandler(TEXT("actor.remove_component"), Payload));
    return true;
}

// ============================================================================
// ComponentHandler — actor.get_component_property
// ============================================================================

// actorName, componentName, and propertyName are all RPC_PARAM_REQ.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentPropertyMissingParamsTest,
    "PinWright.actor.get_component_property.MissingParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentPropertyMissingParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.get_component_property handler is registered"),
        InvokeHandler(TEXT("actor.get_component_property"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentPropertyValidParamsTest,
    "PinWright.actor.get_component_property.ValidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentPropertyValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("NonExistentActor"));
    Payload->SetStringField(TEXT("componentName"), TEXT("StaticMeshComponent0"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Mobility"));
    TestTrue(TEXT("actor.get_component_property handler is registered"),
        InvokeHandler(TEXT("actor.get_component_property"), Payload));
    return true;
}

// ============================================================================
// Response Capture: actor.spawn_from_blueprint with missing required param
// Demonstrates InvokeHandlerWithCapture() pattern for verifying error shape.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSpawnFromBlueprintCaptureErrorTest,
    "PinWright.actor.spawn_from_blueprint.CaptureErrorResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSpawnFromBlueprintCaptureErrorTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Omit "blueprintPath" to trigger the handler's INVALID_ARGUMENT error path.
    bool bFound = InvokeHandlerWithCapture(TEXT("actor.spawn_from_blueprint"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error (missing blueprintPath)"), Capture.bSuccess);
    TestFalse(TEXT("Error code is non-empty"), Capture.ErrorCode.IsEmpty());
    return true;
}

// ============================================================================
// ActorSelectionHandler — actor.select
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSelectMissingParamsTest,
    "PinWright.actor.select.MissingRequiredParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSelectMissingParamsTest::RunTest(const FString& Parameters)
{
    // Neither slot is an RPC_PARAM_REQ (ActorSelectionHandler.cpp:27-30 declares both
    // actorName and actorNames optional), so the dispatcher's required-param gate never
    // fires here despite the test's name. The real guard is in-body: the
    // CollectActorNames branch at ActorSelectionHandler.cpp:47-52 refuses a payload that
    // supplied neither identity slot. Pin its code - a bare TestFalse(bSuccess) is also
    // satisfied by the NO_EDITOR early-out at :33 and by a handler that never responded
    // at all, so it could not tell that guard's deletion from a headless run.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("actor.select handler is registered"), InvokeHandlerWithCapture(TEXT("actor.select"), Payload, Capture));
    TestTrue(TEXT("actor.select responded"), Capture.bWasCalled);
    TestFalse(TEXT("Should fail with missing params"), Capture.bSuccess);
    TestEqual(TEXT("actor.select error code is INVALID_ARGUMENT, not NO_EDITOR"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSelectWithNamesTest,
    "PinWright.actor.select.WithActorNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSelectWithNamesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Names;
    Names.Add(MakeShared<FJsonValueString>(TEXT("TestActor_001")));
    Names.Add(MakeShared<FJsonValueString>(TEXT("TestActor_002")));
    Payload->SetArrayField(TEXT("actorNames"), Names);
    TestTrue(TEXT("actor.select handler is registered"), InvokeHandler(TEXT("actor.select"), Payload));
    return true;
}

// Regression test for E-actor-select-singular-actorname-rejected: actor.select must
// accept the singular actorName scalar that every other per-actor actor.* verb uses,
// in addition to the actorNames array — mirroring the dual-accept already shipped on
// actor.delete. Spawns a real actor into the editor world, then selects it with the
// SINGULAR key actor.select{actorName:"X"} (the exact form the audited agent reached
// for), and asserts the selection succeeds and reports that one actor. If the fix were
// reverted to the array-only RPC_PARAM_REQ("actorNames") spec, the singular call would
// be hard-rejected by the dispatcher's MISSING_REQUIRED_PARAM gate (or, with both
// params optional but no scalar fallback, by the handler's INVALID_ARGUMENT path),
// failing the success assertion below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSelectSingularActorNameTest,
    "PinWright.actor.select.SingularActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSelectSingularActorNameTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping singular actor.select test."));
        return true;
    }

    // Spawn a real source actor; the guard destroys it and restores the dirty flag.
    FScopedEditorWorldActorGuard WorldGuard;
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"), TEXT("SelectSingularSource"));
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        TestTrue(TEXT("actor.spawn succeeded"), SpawnCapture.bSuccess);
    }

    // Select it using the SINGULAR actorName key (not actorNames:["..."]).
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("SelectSingularSource"));
    TestTrue(TEXT("actor.select handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.select"), Payload, Capture));

    TestTrue(TEXT("Handler called SendSuccess or SendError"), Capture.bWasCalled);
    TestTrue(TEXT("Singular actorName is accepted (no INVALID_ARGUMENT/MISSING_REQUIRED_PARAM)"),
        Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double SelectedCount = 0.0;
        Capture.Result->TryGetNumberField(TEXT("selectedCount"), SelectedCount);
        TestEqual(TEXT("Singular actorName selects exactly the one named actor"),
            SelectedCount, 1.0);
    }
    return true;
}

// Guard for the empty-array "clears selection" semantics: passing an empty actorNames
// array must NOT trip the new singular fallback (it would otherwise error on a missing
// actorName). The handler must treat a present-but-empty array as "clear selection" and
// succeed with selectedCount 0. If the fallback fired on an empty array, this would
// surface INVALID_ARGUMENT and the success assertion below would fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSelectEmptyArrayClearsTest,
    "PinWright.actor.select.EmptyArrayClears",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSelectEmptyArrayClearsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping empty-array actor.select test."));
        return true;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("actorNames"), TArray<TSharedPtr<FJsonValue>>());
    TestTrue(TEXT("actor.select handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.select"), Payload, Capture));

    TestTrue(TEXT("Empty actorNames array succeeds (clears selection)"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double SelectedCount = 1.0;
        Capture.Result->TryGetNumberField(TEXT("selectedCount"), SelectedCount);
        TestEqual(TEXT("Empty array clears selection (selectedCount 0)"), SelectedCount, 0.0);
    }
    return true;
}

// Regression test for E-actor-verification-actorpath-is-map-path: the standard
// AddActorVerification block (emitted by ~120 actor-mutating verbs, here exercised
// through the ticket's repro verb actor.add_tag) must report actorPath as the
// actor's OBJECT path, not the map/world PACKAGE path. The package path is shared
// by every actor in the map, so it cannot disambiguate which actor was mutated
// when labels collide — the exact situation in the original repro (seven
// PointLights all labelled "TestPointLight").
//
// This spawns a real actor through the production actor.spawn handler, captures
// the live actor's true object path and package path from the world, then invokes
// the production actor.add_tag handler and asserts:
//   - actorPath == the live actor's GetPathName() (the object path), and
//   - actorPath != the actor's package path, and
//   - mapPath (the newly surfaced map package path) == the actor's package path.
// If the AddActorVerification fix were reverted (actorPath set back to
// GetPackage()->GetPathName()), actorPath would equal the package path and the
// first two assertions would fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorVerificationActorPathIsObjectPathTest,
    "PinWright.actor.add_tag.VerificationActorPathIsObjectPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorVerificationActorPathIsObjectPathTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping verification-block actorPath test."));
        return true;
    }

    // Spawn a real actor; the guard destroys it and restores the level's dirty flag.
    FScopedEditorWorldActorGuard WorldGuard;
    const FString UniqueLabel = TEXT("VerifyActorPathProbe");
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"), UniqueLabel);
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        TestTrue(TEXT("actor.spawn succeeded"), SpawnCapture.bSuccess);
    }

    // Resolve the live actor we just spawned to obtain ground-truth paths directly
    // from the engine (not from the response under test).
    AActor* Spawned = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel() == UniqueLabel)
        {
            Spawned = *It;
            break;
        }
    }
    TestNotNull(TEXT("Spawned probe actor is present in the world"), Spawned);
    if (!Spawned)
    {
        return true;
    }

    const FString ExpectedObjectPath = Spawned->GetPathName();
    const FString PackagePath = Spawned->GetPackage()
        ? Spawned->GetPackage()->GetPathName()
        : FString();
    // Sanity: the object path and the package path must differ for a placed actor,
    // otherwise the assertions below would not distinguish the bug from the fix.
    TestNotEqual(TEXT("Object path differs from package path for a placed actor"),
        ExpectedObjectPath, PackagePath);

    // Drive the ticket's repro verb (actor.add_tag), which appends the standard
    // AddActorVerification block.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> TagPayload = MakeShared<FJsonObject>();
    TagPayload->SetStringField(TEXT("actorName"), UniqueLabel);
    TagPayload->SetStringField(TEXT("tag"), TEXT("AmbiguityProbe"));
    TestTrue(TEXT("actor.add_tag handler is registered"),
        InvokeHandlerWithCapture(TEXT("actor.add_tag"), TagPayload, Capture));
    TestTrue(TEXT("actor.add_tag succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ReportedActorPath;
        TestTrue(TEXT("verification block includes actorPath"),
            Capture.Result->TryGetStringField(TEXT("actorPath"), ReportedActorPath));
        // The core assertion: actorPath is the actor OBJECT path, not the map path.
        TestEqual(TEXT("actorPath is the actor object path"),
            ReportedActorPath, ExpectedObjectPath);
        TestNotEqual(TEXT("actorPath is NOT the map package path"),
            ReportedActorPath, PackagePath);

        // The map package path is still surfaced, but under the distinct mapPath field.
        FString ReportedMapPath;
        TestTrue(TEXT("verification block includes mapPath"),
            Capture.Result->TryGetStringField(TEXT("mapPath"), ReportedMapPath));
        TestEqual(TEXT("mapPath is the actor package (map) path"),
            ReportedMapPath, PackagePath);
    }
    return true;
}
