// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// The owned-PIE start every plugin test uses instead of the engine's FStartPIECommand.
//
// FStartPIECommand starts PIE under whatever GameMode the world or the project resolves to, so a
// plugin test ran the host's game inside it: on a Lyra-based host the GameMode loaded a front-end
// experience whose CommonUI switched input to Menu mode and swallowed the keys
// editor.simulate_input was measuring, and an earlier run crashed in a host pawn's
// ReceiveAsyncPhysicsTick (board B-simulate-input-pie-runs-host-gamemode).
// FRequestPlaySessionParams::GameModeOverride is written onto the PIE world's WorldSettings
// before the game instance starts (PlayLevel.cpp, CreateInnerProcessPIEGameInstance), so it holds
// on any editor map and never touches the editor world. AGameModeBase brings the engine's plain
// APlayerController, ADefaultPawn and AHUD. Level-placed actors of a host map still begin play.

#include "CoreMinimal.h"
#include "Editor/UnrealEdEngine.h"
#include "GameFramework/GameModeBase.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditor.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealEdGlobals.h"

// Host editor plugins also react to PIE start, and the GameMode override cannot stop them.
// PixelStreaming2's editor module creates a PIE streamer on FEditorDelegates::PostPIEStarted when
// PixelStreaming2.Editor.AutoStreamPIE is on (the default), and that streamer's input handler logs
// this Error once because GEngine is a UEditorEngine, not a UGameEngine (RTCInputHandler.cpp).
// Declared as exactly one expected error per PIE start, only when that path is live, so every
// owned-PIE test measures the same thing and no test needs a blanket bSuppressLogErrors.
inline void ExpectHostPieStartErrors(FAutomationTestBase& Test)
{
    const IConsoleVariable* AutoStreamPie =
        IConsoleManager::Get().FindConsoleVariable(TEXT("PixelStreaming2.Editor.AutoStreamPIE"));
    if (FModuleManager::Get().IsModuleLoaded(TEXT("PixelStreaming2Editor"))
        && AutoStreamPie && AutoStreamPie->GetBool())
    {
        Test.AddExpectedErrorPlain(TEXT("Cannot set target window - GEngine is not valid."),
            EAutomationExpectedErrorFlags::Contains, 1);
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FStartHostNeutralPieCommand, FAutomationTestBase*, Test);

inline bool FStartHostNeutralPieCommand::Update()
{
    ExpectHostPieStartErrors(*Test);

    FLevelEditorModule& LevelEditorModule =
        FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));

    // Same request FStartPIECommand builds (AutomationEditorCommon.cpp), plus the override.
    FRequestPlaySessionParams Params;
    Params.DestinationSlateViewport = LevelEditorModule.GetFirstActiveViewport();
    Params.GameModeOverride = AGameModeBase::StaticClass();
    if (GUnrealEd->CheckForPlayerStart() == nullptr)
    {
        FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(Params);
    }
    GUnrealEd->RequestPlaySession(Params);
    return true;
}
