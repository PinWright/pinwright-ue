// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins the editor half of the world accessor in Dispatch/PinWrightWorld.h (docs/runtime-module.md):
// GetTargetWorld() is the editor world until PIE runs and the PIE world while it does,
// GetEditorWorldOrNull() is the editor world in both states, GetTargetGameViewport() is the PIE
// viewport or null, and an editor is never a game process. Moved handlers keep their editor
// semantics by calling these, so a drift here retargets every one of them at once.
// The game half (-game, packaged) is a documented manual check: docs/wiki-src/game-mode.md.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dispatch/PinWrightWorld.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/Drive/HostNeutralPie.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/PieState.h"

namespace PinWrightTargetWorldTest
{
    struct FState
    {
        double Deadline = 0.0;
        double CleanupDeadline = 0.0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetWorldEditorWithoutPieTest,
    "PinWright.infra.target_world.EditorWorldWithoutPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTargetWorldEditorWithoutPieTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor"), TEXT("GEditor is null."));
        return true;
    }
    if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The no-PIE half needs an editor with no PIE session; the test never stops ambient PIE."));
        return true;
    }
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    TestNotNull(TEXT("fixture: the editor has an editor world"), EditorWorld);
    TestTrue(TEXT("no PIE: GetTargetWorld is the editor world"), PinWright::GetTargetWorld() == EditorWorld);
    TestTrue(TEXT("no PIE: GetEditorWorldOrNull is the editor world"),
             PinWright::GetEditorWorldOrNull() == EditorWorld);
    TestNull(TEXT("no PIE: no game viewport"), PinWright::GetTargetGameViewport());
    TestFalse(TEXT("an editor is not a game process"), PinWright::IsGameProcess());
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCheckTargetWorldDuringPie,
    FAutomationTestBase*, Test, TSharedRef<PinWrightTargetWorldTest::FState>, State);

bool FCheckTargetWorldDuringPie::Update()
{
    UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
    UGameViewportClient* Viewport = PlayWorld ? PlayWorld->GetGameViewport() : nullptr;
    if (!Viewport)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_unavailable"),
            TEXT("The owned PIE session did not create PlayWorld with a game viewport."));
        return true;
    }
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    Test->TestTrue(TEXT("fixture: PIE world and editor world differ"), PlayWorld != EditorWorld);
    Test->TestTrue(TEXT("PIE: GetTargetWorld is the PIE world"), PinWright::GetTargetWorld() == PlayWorld);
    Test->TestTrue(TEXT("PIE: GetEditorWorldOrNull stays the editor world, never PIE"),
                   PinWright::GetEditorWorldOrNull() == EditorWorld);
    Test->TestTrue(TEXT("PIE: GetTargetGameViewport is the PIE world's viewport"),
                   PinWright::GetTargetGameViewport() == Viewport);
    Test->TestFalse(TEXT("PIE does not make the editor a game process"), PinWright::IsGameProcess());
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupTargetWorldPie,
    FAutomationTestBase*, Test, TSharedRef<PinWrightTargetWorldTest::FState>, State);

bool FCleanupTargetWorldPie::Update()
{
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return true;
    }
    if (State->CleanupDeadline == 0.0)
    {
        State->CleanupDeadline = FPlatformTime::Seconds() + 15.0;
    }
    if (FPlatformTime::Seconds() >= State->CleanupDeadline)
    {
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
        return true;
    }
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetWorldPieTest,
    "PinWright.infra.target_world.PieWorldWhilePieRuns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTargetWorldPieTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !FSlateApplication::IsInitialized() || !GEditor->GetActiveViewport())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("owned_pie_unavailable"),
            TEXT("Owned PIE requires GEditor, Slate and an active level viewport."));
        return true;
    }
    if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The test never borrows or stops ambient PIE."));
        return true;
    }

    const TSharedRef<PinWrightTargetWorldTest::FState> State = MakeShared<PinWrightTargetWorldTest::FState>();
    State->Deadline = FPlatformTime::Seconds() + 15.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FCheckTargetWorldDuringPie(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupTargetWorldPie(this, State));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
