// Copyright (c) 2026 Alexander Penkin. MIT License.

// Acceptance/regression test for board ticket F-game-features-live-fixture.
//
// Generates a disposable synthetic Game Feature plugin (a real on-disk .uplugin +
// UGameFeatureData carrying one action, created and deleted at test time — see
// GameFeaturesTestFixture.h) and drives it through the REAL UGameFeaturesSubsystem
// lifecycle to Active. It asserts the plugin:
//   * reaches EGameFeaturePluginState::Active via the latent ChangeGameFeatureTargetState,
//   * exposes a NON-EMPTY UGameFeatureData::GetActions() (declared actions readback), and
//   * appears in the production game_features.list RPC while active,
// then deactivates + terminates it, deletes the on-disk plugin, and asserts no leaked
// active state. This proves the game_features.* surface can be exercised against a live
// GF plugin on a fuzz host with zero committed content and no Lyra — the prerequisite
// that unblocks F-game-features-set-state-and-actions (set_state / get_actions).

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Tests/World/GameFeaturesTestFixture.h"

#if MCP_TEST_HAS_GAMEFEATURES

#include "Engine/Engine.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "GameFeaturePluginOperationResult.h"
#include "GameFeatureTypes.h"

#include "Tests/TestUtils.h"

// Latent commands mirror the engine's own GameFeaturePluginTests.cpp pattern: the GFP
// state machine advances across real engine frames, so the async Registered->Active
// round-trip must be driven with latent commands, not a manual ticker pump.

// Waits until a shared bool flips true (an async subsystem transition completed).
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FPinWrightGFWaitForBool, TSharedRef<bool>, bFlag);
bool FPinWrightGFWaitForBool::Update()
{
    return *bFlag;
}

// Runs one step, then completes.
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FPinWrightGFStep, TFunction<void()>, Step);
bool FPinWrightGFStep::Update()
{
    Step();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFeaturesLiveFixtureTest,
    "PinWright.game_features.fixture.SyntheticPluginReachesActiveWithActions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFeaturesLiveFixtureTest::RunTest(const FString& Parameters)
{
    UGameFeaturesSubsystem* Subsystem = GEngine
        ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>()
        : nullptr;
    if (!TestNotNull(TEXT("GameFeatures engine subsystem available"), Subsystem))
    {
        return true;
    }

    // Generate the disposable synthetic GF plugin (uplugin + UGameFeatureData + one action).
    TSharedRef<PinWrightGF::FSyntheticGFPlugin> Plugin =
        MakeShared<PinWrightGF::FSyntheticGFPlugin>();
    FString CreateError;
    const bool bCreated = PinWrightGF::CreateOnDiskGFPluginWithAction(*Plugin, CreateError);
    if (!TestTrue(FString::Printf(TEXT("generated synthetic GF plugin (%s)"), *CreateError), bCreated))
    {
        PinWrightGF::DeleteOnDiskGFPlugin(*Plugin);
        return true;
    }

    const FString URL = Plugin->PluginURL;

    // Drive Installed->Registered->Loaded->Active via the real latent API.
    TSharedRef<bool> bActivateDone = MakeShared<bool>(false);
    TSharedRef<bool> bActivateOk = MakeShared<bool>(false);
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFStep([this, Subsystem, URL, bActivateDone, bActivateOk]()
    {
        Subsystem->ChangeGameFeatureTargetState(URL, EGameFeatureTargetState::Active,
            FGameFeaturePluginChangeStateComplete::CreateLambda(
                [this, bActivateDone, bActivateOk](const UE::GameFeatures::FResult& Result)
                {
                    *bActivateOk = !Result.HasError();
                    *bActivateDone = true;
                    TestTrue(FString::Printf(TEXT("activation transition succeeded (%s)"),
                        *UE::GameFeatures::ToString(Result)), !Result.HasError());
                }));
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFWaitForBool(bActivateDone));

    // Core acceptance: Active + non-empty declared-actions readback + visible on game_features.list.
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFStep([this, Subsystem, URL, Plugin, bActivateOk]()
    {
        if (!*bActivateOk)
        {
            return; // activation already recorded a failure above
        }

        TestEqual(TEXT("plugin reached Active"),
            (int32)Subsystem->GetPluginState(URL), (int32)EGameFeaturePluginState::Active);

        const UGameFeatureData* GFD = Subsystem->GetGameFeatureDataForActivePluginByURL(URL);
        if (TestNotNull(TEXT("active plugin exposes UGameFeatureData"), GFD))
        {
            const int32 NumActions = GFD->GetActions().Num();
            AddInfo(FString::Printf(TEXT("UGameFeatureData::GetActions().Num()=%d"), NumActions));
            TestTrue(TEXT("UGameFeatureData::GetActions() is non-empty (declared actions readable)"),
                NumActions > 0);
        }

        // The production game_features.list RPC must now enumerate the live plugin.
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        FTestResponseCapture Capture;
        if (TestTrue(TEXT("game_features.list handler found"),
                InvokeHandlerWithCapture(TEXT("game_features.list"), Payload, Capture))
            && TestTrue(TEXT("game_features.list succeeded"), Capture.bSuccess)
            && Capture.Result.IsValid())
        {
            TestTrue(TEXT("game_features.list enumerates the live fixture plugin by name"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("plugins"),
                    TEXT("name"), Plugin->PluginName));
        }
    }));

    // Teardown: deactivate (if active) then terminate through the real subsystem.
    TSharedRef<bool> bDeactivateDone = MakeShared<bool>(false);
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFStep([Subsystem, URL, bDeactivateDone]()
    {
        if (Subsystem->GetPluginState(URL) >= EGameFeaturePluginState::Active)
        {
            Subsystem->DeactivateGameFeaturePlugin(URL,
                FGameFeaturePluginChangeStateComplete::CreateLambda(
                    [bDeactivateDone](const UE::GameFeatures::FResult&) { *bDeactivateDone = true; }));
        }
        else
        {
            *bDeactivateDone = true;
        }
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFWaitForBool(bDeactivateDone));

    TSharedRef<bool> bTerminateDone = MakeShared<bool>(false);
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFStep([Subsystem, URL, bTerminateDone]()
    {
        if (Subsystem->GetPluginState(URL) > EGameFeaturePluginState::Uninstalled)
        {
            Subsystem->TerminateGameFeaturePlugin(URL,
                FGameFeaturePluginChangeStateComplete::CreateLambda(
                    [bTerminateDone](const UE::GameFeatures::FResult&) { *bTerminateDone = true; }));
        }
        else
        {
            *bTerminateDone = true;
        }
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFWaitForBool(bTerminateDone));

    // No leaked state, then delete the on-disk plugin folder. The ticket's acceptance defines
    // the no-leak criterion as game_features.list returning to the pre-fixture set, so assert
    // both that no ACTIVE data remains AND that the production RPC no longer enumerates the
    // plugin by name — a machine left lingering in a non-active state would pass the
    // active-data check yet still show up here (and leak its mounted content into siblings).
    ADD_LATENT_AUTOMATION_COMMAND(FPinWrightGFStep([this, Subsystem, URL, Plugin]()
    {
        TestNull(TEXT("no leaked active state after teardown"),
            Subsystem->GetGameFeatureDataForActivePluginByURL(URL));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        FTestResponseCapture Capture;
        if (TestTrue(TEXT("game_features.list handler found (post-teardown)"),
                InvokeHandlerWithCapture(TEXT("game_features.list"), Payload, Capture))
            && TestTrue(TEXT("game_features.list succeeded (post-teardown)"), Capture.bSuccess)
            && Capture.Result.IsValid())
        {
            TestFalse(TEXT("game_features.list no longer enumerates the terminated fixture plugin"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("plugins"),
                    TEXT("name"), Plugin->PluginName));
        }

        PinWrightGF::DeleteOnDiskGFPlugin(*Plugin);
    }));

    return true;
}

#endif // MCP_TEST_HAS_GAMEFEATURES
