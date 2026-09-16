// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the system.inspect.* game-framework singleton handlers
// registered in Handlers/Environment/SystemInspectSingletonsHandler.cpp.
// Tests run in the editor without PIE active, so they exercise the no-world
// (singleton) and empty-array (collection) code paths.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

// ============================================================================
// system.inspect.get_game_mode — no PIE, expect typed GAME_MODE_NOT_FOUND error.
// Counterfactual: removing the null-world guard would crash on World->GetAuthGameMode().
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectGetGameModeNoPieReturnsNotFoundTest,
    "PinWright.system.inspect.get_game_mode.NoPieReturnsNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectGetGameModeNoPieReturnsNotFoundTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(
        TEXT("system.inspect.get_game_mode handler found"),
        InvokeHandlerWithCapture(TEXT("system.inspect.get_game_mode"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("handler returned an error (not success)"), Capture.bSuccess);
    TestEqual(TEXT("error code is GAME_MODE_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("GAME_MODE_NOT_FOUND")));
    return true;
}

// ============================================================================
// system.inspect.get_player_controllers — no PIE, expect success + empty array.
// Counterfactual: iterating a null world via GetPlayerControllerIterator() would crash.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectGetPlayerControllersNoPieReturnsEmptyTest,
    "PinWright.system.inspect.get_player_controllers.NoPieReturnsEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectGetPlayerControllersNoPieReturnsEmptyTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(
        TEXT("system.inspect.get_player_controllers handler found"),
        InvokeHandlerWithCapture(TEXT("system.inspect.get_player_controllers"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("handler succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* PlayerControllers = nullptr;
    const bool bHasArray =
        Capture.Result->TryGetArrayField(TEXT("playerControllers"), PlayerControllers);
    TestTrue(TEXT("result has playerControllers array"), bHasArray);
    if (bHasArray && PlayerControllers)
    {
        TestEqual(TEXT("playerControllers array is empty outside PIE"),
            PlayerControllers->Num(), 0);
    }
    return true;
}
