// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for WaterHandler.cpp (water.* namespace).
//
// Registration sanity test compiles in both gated states — proves the
// namespace appears in discovery even when the Water plugin is disabled,
// matching the ticket's "still appear in discovery" requirement.
//
// The functional spawn test is gated on MCP_HAS_WATER because it references
// AWaterBodyLake symbols only available when the Water plugin is enabled.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Misc/Guid.h"
#include "UObject/UObjectGlobals.h"

#if __has_include("WaterBodyActor.h")
#include "WaterBodyActor.h"
#include "WaterBodyLakeActor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#define MCP_HAS_WATER 1
#else
#define MCP_HAS_WATER 0
#endif

// ============================================================================
// water.spawn_water_body — registration counterfactual
// ----------------------------------------------------------------------------
// Counterfactual: if WaterHandler.cpp is reverted or removed, the dispatcher
// returns METHOD_NOT_FOUND for water.spawn_water_body and InvokeHandler() in
// this test returns false. Holds in both MCP_HAS_WATER states because
// registration is unconditional.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterMethodNotFoundCounterfactualTest,
    "PinWright.water.spawn_water_body.RegisteredEvenWhenGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWaterMethodNotFoundCounterfactualTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("water.spawn_water_body is registered"),
        IsHandlerRegistered(TEXT("water.spawn_water_body")));
    TestTrue(TEXT("water.spawn_water_zone is registered"),
        IsHandlerRegistered(TEXT("water.spawn_water_zone")));
    TestTrue(TEXT("water.set_water_body_material is registered"),
        IsHandlerRegistered(TEXT("water.set_water_body_material")));
    TestTrue(TEXT("water.set_water_body_underwater_post_process is registered"),
        IsHandlerRegistered(TEXT("water.set_water_body_underwater_post_process")));

    // Confirm the dispatcher actually invokes the handler (handler reports the
    // missing 'type' parameter via Ctx.SendError — InvokeHandler returns true
    // because the handler ran, regardless of payload validity).
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("water.spawn_water_body handler invocable"),
        InvokeHandler(TEXT("water.spawn_water_body"), Payload));
    return true;
}

// ============================================================================
// water.spawn_water_body — functional spawn test (Water plugin required)
// ============================================================================

#if MCP_HAS_WATER

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterSpawnWaterBodyValidParamsTest,
    "PinWright.water.spawn_water_body.SpawnsLakeAndDestroys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWaterSpawnWaterBodyValidParamsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping spawn assertion"));
        return true;
    }

    TSet<AWaterBodyLake*> LakesBefore;
    for (TActorIterator<AWaterBodyLake> It(World); It; ++It)
    {
        LakesBefore.Add(*It);
    }

    AWaterBodyLake* Spawned = nullptr;
    {
        FScopedEditorWorldActorGuard WorldGuard;
        const FString LakeLabel = FString::Printf(TEXT("McpTestLake_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("type"), TEXT("Lake"));
        Payload->SetStringField(TEXT("name"), LakeLabel);

        FTestResponseCapture Capture;
        TestTrue(TEXT("water.spawn_water_body handler found"),
            InvokeHandlerWithCapture(TEXT("water.spawn_water_body"), Payload, Capture));
        TestTrue(TEXT("water.spawn_water_body responded"), Capture.bWasCalled);
        TestTrue(TEXT("water.spawn_water_body succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return true;
        }

        FString ActorPath;
        TestTrue(TEXT("water.spawn_water_body returned actorPath"),
            Capture.Result->TryGetStringField(TEXT("actorPath"), ActorPath));
        Spawned = ActorPath.IsEmpty() ? nullptr : FindObject<AWaterBodyLake>(nullptr, *ActorPath);

        TestNotNull(TEXT("AWaterBodyLake was spawned"), Spawned);
        if (!Spawned)
        {
            return true;
        }
        TestTrue(TEXT("Spawned actor is AWaterBodyLake"),
            Spawned->IsA(AWaterBodyLake::StaticClass()));

        FString ReturnedLabel;
        TestTrue(TEXT("water.spawn_water_body returned actorLabel"),
            Capture.Result->TryGetStringField(TEXT("actorLabel"), ReturnedLabel));
        TestEqual(TEXT("water.spawn_water_body used the GUID-suffixed fixture label"),
            ReturnedLabel, LakeLabel);

        TestTrue(TEXT("returned actorPath resolves in the active editor world"),
            Spawned->GetWorld() == World);

        TSet<AWaterBodyLake*> LakesAfterSpawn;
        for (TActorIterator<AWaterBodyLake> It(World); It; ++It)
        {
            LakesAfterSpawn.Add(*It);
        }
        TestEqual(TEXT("the spawn added exactly one lake fixture"),
            LakesAfterSpawn.Num(), LakesBefore.Num() + 1);
        TestTrue(TEXT("the returned actorPath identifies the added lake"),
            LakesAfterSpawn.Contains(Spawned) && !LakesBefore.Contains(Spawned));
        for (AWaterBodyLake* Lake : LakesBefore)
        {
            TestTrue(TEXT("every pre-existing lake remains in the world after spawn"),
                IsValid(Lake) && LakesAfterSpawn.Contains(Lake));
        }
    }

    TSet<AWaterBodyLake*> LakesAfterCleanup;
    for (TActorIterator<AWaterBodyLake> It(World); It; ++It)
    {
        LakesAfterCleanup.Add(*It);
    }
    TestEqual(TEXT("lake count is unchanged after guarded cleanup"),
        LakesAfterCleanup.Num(), LakesBefore.Num());
    for (AWaterBodyLake* Lake : LakesBefore)
    {
        TestTrue(TEXT("guarded cleanup preserves each original lake identity"),
            IsValid(Lake) && LakesAfterCleanup.Contains(Lake));
    }
    return true;
}

// ============================================================================
// water.set_water_body_underwater_post_process — silent-drop regression
// ----------------------------------------------------------------------------
// Ticket E-water-underwater-settings-silent-drop: passing settings keys that do
// not resolve to a direct field of FUnderwaterPostProcessSettings (e.g. the flat
// FPostProcessSettings field names SceneColorTint/bOverride_SceneColorTint/
// FogDensity, which only stick when nested under PostProcessSettings) used to be
// silently dropped with a clean ok:true / applied:[]. The fix validates before
// mutating and rejects the whole call with INVALID_PARAMS + a droppedSettings
// list. Counterfactual: reverting WaterHandler.cpp's pre-scan to the old
// `if (!InnerProp) continue;` silent drop turns this back into an empty-applied
// success, flipping bSuccess to true and failing every assertion below.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterUnderwaterSettingsSilentDropTest,
    "PinWright.water.set_water_body_underwater_post_process.RejectsUnmatchedSettingsKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWaterUnderwaterSettingsSilentDropTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping underwater-settings assertion"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString LakeLabel = FString::Printf(TEXT("McpUnderwaterDropLake_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Spawn a lake to act on; use the response's canonical actor path below.
    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("type"), TEXT("Lake"));
    SpawnPayload->SetStringField(TEXT("name"), LakeLabel);
    FTestResponseCapture SpawnCapture;
    TestTrue(TEXT("water.spawn_water_body handler found"),
        InvokeHandlerWithCapture(TEXT("water.spawn_water_body"), SpawnPayload, SpawnCapture));
    TestTrue(TEXT("water.spawn_water_body responded"), SpawnCapture.bWasCalled);
    TestTrue(TEXT("water.spawn_water_body succeeded"), SpawnCapture.bSuccess);

    FString ActorPath;
    TestTrue(TEXT("water.spawn_water_body returned actorPath"),
        SpawnCapture.Result.IsValid() &&
            SpawnCapture.Result->TryGetStringField(TEXT("actorPath"), ActorPath));
    AWaterBodyLake* Spawned = ActorPath.IsEmpty()
        ? nullptr
        : FindObject<AWaterBodyLake>(nullptr, *ActorPath);
    TestNotNull(TEXT("Test lake was spawned"), Spawned);
    if (!Spawned)
    {
        return true;
    }
    TestTrue(TEXT("returned actorPath resolves in the active editor world"),
        Spawned->GetWorld() == World);

    // Flat (obvious-but-wrong) form: real FPostProcessSettings field names passed
    // directly. None of these resolve to a direct field of
    // FUnderwaterPostProcessSettings — the silent-drop bug returned applied:[] / ok.
    TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
    Settings->SetBoolField(TEXT("bOverride_SceneColorTint"), true);
    Settings->SetNumberField(TEXT("FogDensity"), 0.05);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor"), ActorPath);
    Payload->SetObjectField(TEXT("settings"), Settings);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("water.set_water_body_underwater_post_process"), Payload, Capture);
    TestTrue(TEXT("handler invoked"), bFound);

    // The whole call must be rejected — not a clean success with dropped keys.
    TestFalse(TEXT("response is an error, not a misleading success"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARAMS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));

    // The dropped keys must be reported in a droppedSettings array (objects with a
    // 'key' field), not evaporated.
    TestTrue(TEXT("droppedSettings lists bOverride_SceneColorTint"),
        JsonArrayHasObjectWithStringField(Capture.Result, TEXT("droppedSettings"),
            TEXT("key"), TEXT("bOverride_SceneColorTint")));
    TestTrue(TEXT("droppedSettings lists FogDensity"),
        JsonArrayHasObjectWithStringField(Capture.Result, TEXT("droppedSettings"),
            TEXT("key"), TEXT("FogDensity")));

    return true;
}

#endif // MCP_HAS_WATER
