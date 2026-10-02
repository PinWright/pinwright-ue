// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.console_command world defaulting and player-exec routing.
//   - E-console-command-editor-default-during-pie: an omitted `world` ran in the editor world
//     while a PIE session was running, so a game command answered `consumed` and did nothing.
//   - E-console-command-player-exec: PlayerController / CheatManager exec commands (EnableCheats,
//     summon) never reached the player chain and failed EXEC_FAILED in a PIE world.
// OmittedWorldResolve covers the pure default rule (including the ambiguous several-PIE case, which
// needs no PIE session). PiePlayerExecOmittedWorld owns a standalone PIE session and drives the
// registered handler through both fixes at once.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CheatManager.h"
#include "GameFramework/PlayerController.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/Drive/HostNeutralPie.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/PieState.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandOmittedWorldResolveTest,
    "PinWright.editor.console_command.OmittedWorldResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandOmittedWorldResolveTest::RunTest(const FString& Parameters)
{
    using namespace PieWorldSelector;

    FPieContextInfo Server;
    Server.PieInstance = 0;
    Server.NetMode = NM_ListenServer;
    FPieContextInfo Client;
    Client.PieInstance = 1;
    Client.NetMode = NM_Client;

    TestEqual(TEXT("no PIE world: omitted world is the editor world"),
        (int32)ResolveOmitted({}), (int32)EOmittedWorld::Editor);
    TestEqual(TEXT("one PIE world: omitted world is that PIE world"),
        (int32)ResolveOmitted({Server}), (int32)EOmittedWorld::SolePie);
    TestEqual(TEXT("several PIE worlds: omitted world is ambiguous, never a pick"),
        (int32)ResolveOmitted({Server, Client}), (int32)EOmittedWorld::Ambiguous);
    return true;
}

namespace ConsoleCommandPieRoutingTest
{
    struct FState
    {
        double Deadline = 0.0;
        double CleanupDeadline = 0.0;
    };

    TSharedPtr<FJsonObject> Payload(const TCHAR* Command, const TCHAR* World = nullptr)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("command"), Command);
        if (World)
        {
            Json->SetStringField(TEXT("world"), World);
        }
        return Json;
    }

    FString GetString(const FTestResponseCapture& Capture, const TCHAR* Field)
    {
        FString Value;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(Field, Value);
        }
        return Value;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRunConsoleCommandPieRouting,
    FAutomationTestBase*, Test, TSharedRef<ConsoleCommandPieRoutingTest::FState>, State);

bool FRunConsoleCommandPieRouting::Update()
{
    using namespace ConsoleCommandPieRoutingTest;
    UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
    ULocalPlayer* LocalPlayer = PlayWorld ? GEngine->GetFirstGamePlayer(PlayWorld) : nullptr;
    APlayerController* PC = LocalPlayer ? LocalPlayer->PlayerController.Get() : nullptr;
    if (!PC)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_player_unavailable"),
            TEXT("The owned PIE session did not create PlayWorld with a local player controller."));
        return true;
    }

    // Explicit 'editor' still means the editor world while PIE runs: EnableCheats has no handler
    // there, and the refusal names where player exec commands live.
    FTestResponseCapture EditorCapture;
    Test->TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("editor.console_command"), Payload(TEXT("EnableCheats"), TEXT("editor")), EditorCapture));
    Test->TestFalse(TEXT("EnableCheats in the explicit editor world is not consumed"), EditorCapture.bSuccess);
    Test->TestEqual(TEXT("explicit editor refusal code"), EditorCapture.ErrorCode, FString(ErrorCodes::ERR_EXEC_FAILED));
    Test->TestTrue(TEXT("refusal points at a PIE world"), EditorCapture.Message.Contains(TEXT("PIE world")));

    // World omitted with one PIE world running: resolves to it (fix 1), and EnableCheats reaches
    // the PlayerController through the local player (fix 2). Read the effect back: PIE already
    // spawns a CheatManager, so clear it first and require EnableCheats to recreate it.
    PC->CheatManager = nullptr;
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("editor.console_command"), Payload(TEXT("EnableCheats")), Capture);
    Test->TestTrue(FString::Printf(TEXT("EnableCheats with world omitted succeeds (%s: %s)"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    Test->TestEqual(TEXT("omitted world resolves to the PIE world"),
        GetString(Capture, TEXT("worldPath")), PlayWorld->GetPathName());
    Test->TestTrue(TEXT("resolved world is echoed as a pie:N selector"),
        GetString(Capture, TEXT("world")).StartsWith(TEXT("pie:")));
    bool bDefaulted = false;
    Test->TestTrue(TEXT("worldDefaulted is reported"),
        Capture.Result.IsValid() && Capture.Result->TryGetBoolField(TEXT("worldDefaulted"), bDefaulted) && bDefaulted);
    Test->TestEqual(TEXT("consumed through the local player"), GetString(Capture, TEXT("route")), FString(TEXT("localPlayer")));
    Test->TestEqual(TEXT("names the receiving player controller"), GetString(Capture, TEXT("playerController")), PC->GetName());
    Test->TestNotNull(TEXT("EnableCheats recreated the CheatManager"), PC->CheatManager.Get());
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupConsoleCommandPieRouting,
    FAutomationTestBase*, Test, TSharedRef<ConsoleCommandPieRoutingTest::FState>, State);

bool FCleanupConsoleCommandPieRouting::Update()
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandPiePlayerExecTest,
    "PinWright.editor.console_command.PiePlayerExecOmittedWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandPiePlayerExecTest::RunTest(const FString& Parameters)
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

    const TSharedRef<ConsoleCommandPieRoutingTest::FState> State = MakeShared<ConsoleCommandPieRoutingTest::FState>();
    State->Deadline = FPlatformTime::Seconds() + 15.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FRunConsoleCommandPieRouting(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupConsoleCommandPieRouting(this, State));
    return true;
}
