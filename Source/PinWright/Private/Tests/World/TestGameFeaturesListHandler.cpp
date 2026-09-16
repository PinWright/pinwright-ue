// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for board ticket F-game-features-plugin-management.
// The Game Features subsystem (UGameFeaturesSubsystem) gates whole gameplay
// slices behind GF plugins on Lyra-derived hosts, but PinWright exposes NO RPC
// over it: there are zero `game_features.*` handlers. The `game_framework.*`
// namespace manages GameMode/GameState classes, an unrelated concern. This test
// pins the proposed read-only `game_features.list` verb (the ticket's first
// acceptance surface): it must be registered AND, when invoked, run against the
// GameFeatures subsystem and succeed, returning the registered GF plugins as a
// `plugins` array. Pre-fix the handler is absent, so registration + invocation
// fail — that absence is the reproduction. The `plugins`-array key encodes the
// acceptance shape the implementer adopts on GO.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

// ============================================================================
// game_features.list — must exist and enumerate registered GF plugins.
// The registration + invoke-success assertions are key-agnostic and fail pre-fix
// simply because no game_features.* handler is registered (differential proof).
// The plugins-array check (guarded on success) proves the list shape post-fix.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFeaturesListHandlerListsPluginsTest,
    "PinWright.game_features.list.RegisteredAndListsPlugins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFeaturesListHandlerListsPluginsTest::RunTest(const FString& Parameters)
{
    // The verb must be discoverable on the RPC surface.
    TestTrue(TEXT("game_features.list handler is registered"),
        IsHandlerRegistered(TEXT("game_features.list")));

    // Invoking it must run against UGameFeaturesSubsystem and succeed, returning
    // the registered GF plugins as a `plugins` array (empty on a host with no GF
    // plugins, but the field must be present). A NOT_IMPLEMENTED stub fails here.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("game_features.list"), Payload, Capture);
    TestTrue(TEXT("game_features.list handler found for invocation"), bFound);
    TestTrue(TEXT("game_features.list responded"), Capture.bWasCalled);
    TestTrue(TEXT("game_features.list succeeded (not an error / NOT_IMPLEMENTED stub)"),
        Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Plugins = nullptr;
        TestTrue(TEXT("result carries a plugins array"),
            Capture.Result->TryGetArrayField(TEXT("plugins"), Plugins));
    }
    return true;
}
