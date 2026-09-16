// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// The fifth contamination channel: text the ENGINE draws into the frame, which no show flag, game
// view or hideEditorSprites removes and which every other honesty field reads clean through.
//
// Measured cost (board E-capture-blind-to-renderer-error-banner, twice): ten acceptance frames came
// back with "Video memory has been exhausted (1197.488 MB over budget). Expect extremely poor
// performance." burned across the upper third in red, while `showFlagOverrides.forced` was empty,
// `editorSprites.visible` false, `gameView` true and `blank`/`crushed`/`blownOut` all clean. The
// banner is drawn into scene colour before the readback, so it also skews `imageStats` -- which is
// why this reads the strings rather than guessing from luminance.
//
// WHAT IS AND IS NOT READABLE FROM THE GAME THREAD, because the obvious call does not exist:
//   * `UEngine::GetOnScreenDebugMessages()` is NOT an API. `PriorityScreenMessages` and
//     `ScreenMessages` are private members of UEngine with no accessor and no UPROPERTY, so they
//     cannot be read directly or by reflection (Engine.h:2186-2206, UE 5.8).
//   * `FCoreDelegates::OnGetOnScreenMessages` IS public and is broadcast from the game thread. It
//     carries the severity-tagged family (Nanite feedback, "Lighting needs to be rebuilt"-class
//     notifications, platform status lines), so broadcasting it here collects exactly what the
//     engine would collect when it draws them.
//   * `FSceneRenderer::OnGetOnScreenMessages` -- the renderer's own list, which carries the
//     multiple-directional-light line -- is declared in Renderer/Private/SceneRendering.h and is
//     render-thread only. Unreachable; do not claim to cover it.
//   * The VRAM banner is not on any delegate: SceneRendering.cpp:4802-4806 prints it inline from
//     GDemotedLocalMemorySize under the r.DemotedLocalMemoryWarning cvar. Both are public globals,
//     so the exact condition and the exact string are reproduced here rather than approximated.
namespace PinWrightOnScreenMessages
{
    struct FOnScreenMessage
    {
        // "info" | "warning" | "error"
        FString Severity;
        // Which readable channel it came from: "coreDelegate" or "renderer".
        FString Source;
        FString Text;
        // The colour the ENGINE draws this line in, not a convention chosen here.
        FLinearColor Color = FLinearColor::White;
    };

    struct FOnScreenMessageSurvey
    {
        // False means no survey ran on this path, so the array is omitted rather than published as
        // an empty list nobody measured.
        bool bMeasured = false;
        // The engine's own two gates. With either of these against it the engine draws none of
        // these strings, so an empty list is a positive statement about the frame.
        bool bScreenMessagesEnabled = true;
        bool bMapWarningsSuppressed = false;
        TArray<FOnScreenMessage> Messages;
    };

    // Game thread: FCoreDelegates::OnGetOnScreenMessages is broadcast, so its subscribers run here.
    FOnScreenMessageSurvey Survey();

    // The renderer's VRAM banner, split out as pure functions of the two globals it reads so the
    // exact condition and the exact string can be asserted without a GPU, without a frame, and
    // without writing to GRHIGlobals in a shared editor.
    bool ShouldReportDemotedLocalMemory(uint64 DemotedLocalBytes, int32 WarningCVarValue);
    FString MakeDemotedLocalMemoryText(uint64 DemotedLocalBytes);

    // Attaches `onScreenMessages` (plus `onScreenMessageWarning` when non-empty) to a `viewport`
    // object. A no-op when nothing was surveyed.
    void AddOnScreenMessageFields(const FOnScreenMessageSurvey& Surveyed,
                                  const TSharedPtr<FJsonObject>& Viewport);
}
