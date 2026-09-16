// Copyright (c) 2026 Alexander Penkin. MIT License.

// PieTimeControl - the clock half of editor.pause / editor.resume / editor.step_frame.
//
// WHY THIS EXISTS (board E-pause-step-frame-does-not-freeze-umg). Pausing PIE writes
// UWorld::bDebugPauseExecution, and exactly one thing reads that flag: UWorld::IsPaused
// (LevelTick.cpp:443-451), which gates ACTOR ticking and the FX system. Slate is nowhere in
// that path. Widgets kept ticking off Slate's own real-time clock, so a HUD whose animation
// lives in UUserWidget::Tick or in a UWidgetAnimation ran on while the session reported
// {"state":"paused"} - and ran on through the seconds of RPC round-trip between the call that
// triggered it and the call that captured it. A 210 ms hit marker was therefore uncapturable,
// and the paused frames were worse than empty, they were MISLEADING: the HUD's Tick ran again
// against a frozen world and collapsed the crosshair into a state the running game never
// renders.
//
// THE TWO UI CLOCKS, AND WHY EACH NEEDS ITS OWN LEVER.
//
//   1. UMG ANIMATIONS (UWidgetAnimation / PlayAnimation) advance from
//      UUMGSequenceTickManager::TickWidgetAnimations, bound to FSlateApplication::OnPreTick
//      (UMGSequenceTickManager.cpp:56, forwarding to UUserWidget::TickActionsAndAnimation at
//      :288). That delegate is broadcast out of FSlateApplication::TickAndDrawWidgets with the
//      tick's DeltaTime (SlateApplication.cpp:1758), and that DeltaTime is REPLACED by
//      FSlateApplication::GetFixedDeltaTime() when the Slate.UseFixedDeltaTime cvar is on
//      (SlateApplication.cpp:1658-1661). So the animation clock is settable: cvar on plus
//      SetFixedDeltaTime(0) freezes it, SetFixedDeltaTime(dt) advances it by exactly dt. This
//      is the engine's own recipe for a deterministic UI screenshot -
//      AFunctionalUIScreenshotTest::PrepareTest sets the pair (FunctionalUIScreenshotTest.cpp:
//      108-114) and EndPlay restores it (:154-158).
//
//   2. WIDGET Tick (UUserWidget::NativeTick -> the Blueprint Tick event, widget extensions and
//      that widget's latent actions) does NOT come from that delta. SObjectWidget::Tick is
//      reached from SWidget::Paint with Args.GetDeltaTime() (SWidget.cpp:1511), and the paint
//      args are built in FSlateApplication::DrawWindowAndChildren from
//      PaintWindow(GetCurrentTime(), GetDeltaTime(), ...) (SlateApplication.cpp:1268-1269) -
//      the REAL wall-clock delta, which the cvar does not touch and which no public API can
//      re-point (CurrentTime / LastTickTime are private and TickTime() reads
//      FPlatformTime::Seconds()). The only public lever on that path is the per-widget tick
//      flag SWidget::SetCanTick (SWidget.h:677), which is what UUserWidget::UpdateCanTick
//      itself drives (UserWidget.cpp:2379). So widget Tick is switched OFF for the frozen
//      frames and back ON for the stepped one, rather than slowed down.
//      Restoring is a RECOMPUTE, not a saved value: UpdateCanTick derives the flag from
//      TickFrequency, a script-implemented Tick, live animations, queued transitions, latent
//      actions and extensions (UserWidget.cpp:2347-2380), so calling it is always correct and
//      never needs the pre-freeze value. That also makes the freeze robust against the engine
//      recomputing the flag underneath us - it cannot, because nothing recomputes while the
//      widget is not ticking and the world is paused.
//
// WORLD CLOCK, AND WHY IT NEEDS TWO LEVERS.
//
//   1. PROCESS. FApp::SetUseFixedTimeStep(true) + FApp::SetFixedDeltaTime(dt) makes
//      UEngine::UpdateTimeAndHandleMaxTickRate hand the ENGINE frame exactly dt
//      (UnrealEngine.cpp:3015-3022). Necessary, but on its own it does not size the PIE world
//      tick, because the editor overrides that per world context.
//
//   2. PER PIE WORLD CONTEXT. UEditorEngine::Tick chooses the PIE tick length as
//        if (PieContext.PIEFixedTickSeconds > 0.f) TickDeltaSeconds = PIEFixedTickSeconds;
//        else                                      TickDeltaSeconds = DeltaSeconds;
//      and then ticks the world once per whole PIEFixedTickSeconds sitting in the context's
//      accumulator (EditorEngine.cpp:2133-2146, tick at :2169; field FWorldContext::
//      PIEFixedTickSeconds, Engine.h:442-443). Left at its default 0 the world tick simply
//      takes whatever the editor frame took. That was the reported defect: a step advanced the
//      world ~1/3 s on a loaded editor and ~1/60 s on an idle one, identically for a 0.017 s
//      and a 0.9 s request, because the number was the editor's wall-clock frame and not a
//      function of the request at all.
//      So a step sets this field too and puts back exactly what it found - it can already be
//      non-zero when the session was started with a Client/Server fixed FPS
//      (PlayLevel.cpp:1837-1848).
//
// Both are engaged ONLY across a step, never for the whole pause: while paused the world ticks
// no actors at all, and detaching the process clock from wall time for a pause that lasts
// minutes would make the editor's own time lie for no gain. The engine handles the switch back
// itself (UnrealEngine.cpp:3032-3041 re-bases LastRealTime on the fixed -> real transition), so
// no delta hitch leaks into the resumed session.
//
// WHAT THE WORLD STILL DOES TO THE NUMBER. UWorld::Tick multiplies the delta it is handed by
// AWorldSettings::GetEffectiveTimeDilation() and passes it through
// AWorldSettings::FixupDeltaSeconds, a virtual clamp into [MinUndilatedFrameTime,
// MaxUndilatedFrameTime] that a project can replace (LevelTick.cpp:1593-1611,
// WorldSettings.cpp:334-343). A step therefore asks the level's own world settings what a
// single tick can be before running the frame, and reports that beside what it measured, so a
// caller can tell a step that was honoured from one a floor or a ceiling ate.
//
// MOTION BLUR ACROSS A FREEZE. A session is frozen in order to be photographed, and velocity
// vectors are the one thing a frozen world does not freeze: nothing in it is moving, so any
// motion vector left in the frame describes motion from before the pause and can only be
// wrong. r.MotionBlurQuality is held at 0 for the life of the freeze and restored with the
// clocks.
//
// WHAT IS STILL NOT EXACT, AND CANNOT BE. Across the one stepped frame the widget Tick delta
// is Slate's real frame delta (~1/60 s), because of the widget-Tick clock above: the flag is
// binary, and the clock behind it is not ours. The world and the UMG animations advance by
// exactly the requested dt;
// a HUD that integrates its own alpha in Tick advances by the real frame instead. step_frame
// reports worldSecondsAdvanced and uiSecondsAdvanced rather than pretending they are one
// number, and the wiki says so on the verb.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class AWorldSettings;
struct FWorldContext;

namespace PinWrightPieTime
{

// Default step size: one 60 Hz frame.
inline constexpr double DefaultStepDeltaSeconds = 1.0 / 60.0;

// Upper bound on a single step. Anything larger is almost always a units mistake
// (milliseconds passed as seconds), and one world tick that long is clamped by
// AWorldSettings::FixupDeltaSeconds regardless, so the reported world advance would not
// match the request anyway.
inline constexpr double MaxStepDeltaSeconds = 1.0;

// Pure. Resolves a caller-supplied deltaSeconds (absent / 0 -> DefaultStepDeltaSeconds) and
// rejects non-finite, negative and out-of-range values with a caller-facing reason.
bool ValidateStepDelta(double Requested, double& OutDelta, FString& OutReason);

// ---------------------------------------------------------------------------
// The two clock levers. Each saves the pre-existing value on its first engage and
// restores exactly that on release, so repeated engages only move the delta.
// ---------------------------------------------------------------------------

// UMG animation clock: Slate.UseFixedDeltaTime on + FSlateApplication::SetFixedDeltaTime.
// No-op when Slate is not initialized (headless runs).
void SetUiClockDelta(double Seconds);
void RestoreUiClock();
bool IsUiClockEngaged();

// World clock: the FApp process fixed time step AND the stepped PIE world context's own
// PIEFixedTickSeconds. Engaging without a PIE world sets only the process half.
void SetWorldClockDelta(double Seconds);
void RestoreWorldClock();
bool IsWorldClockEngaged();

// The same two levers against a caller-named world context instead of the stepped one, so the
// suite can drive a context it owns rather than needing a live PIE session. A null Context
// engages the process half only. Re-engaging moves the delta without re-saving.
void EngageWorldClock(double Seconds, FWorldContext* Context);
void ReleaseWorldClock(FWorldContext* Context);

// What one world tick of the requested length can actually be in a given level, read from that
// level's own AWorldSettings before the frame runs.
struct FWorldStepBudget
{
    // Requested * time dilation, then through AWorldSettings::FixupDeltaSeconds.
    double ExpectedSeconds = 0.0;
    double TimeDilation = 1.0;
    // "none", "timeDilation", "worldSettings.MinUndilatedFrameTime" or
    // "worldSettings.MaxUndilatedFrameTime".
    const TCHAR* ClampSource = TEXT("none");
};

// Pure. Names why a step will not buy exactly what was asked, given the dilated delta and the
// delta the world settings will actually allow.
const TCHAR* ClassifyWorldStepClamp(double DilatedSeconds, double AllowedSeconds,
                                    double TimeDilation);

// Runs the level's own (virtual, therefore project-replaceable) clamp rather than a copy of it.
// A null Settings yields the request unchanged with ClampSource "none".
FWorldStepBudget ResolveWorldStepBudget(AWorldSettings* Settings, double RequestedSeconds);

// Motion blur is suppressed for the life of a freeze; see the header comment.
bool IsMotionBlurSuppressed();

// ---------------------------------------------------------------------------
// Session-level freeze
// ---------------------------------------------------------------------------

bool IsFrozen();

// Freeze the UI of every live PIE world: UMG animation clock to 0 and widget Tick off for
// every UUserWidget owned by a PIE world, plus r.MotionBlurQuality held at 0 so the frames
// captured off the frozen session carry no pre-pause motion. Editor-owned widgets are never
// touched. Also
// subscribes to FEditorDelegates::ResumePIE / EndPIE so a resume or stop driven from the
// editor's own toolbar releases the freeze instead of stranding dead HUD widgets.
// The animation clock is process-global, so callers freeze only with a session running -
// the two verbs that call this both guard on GEditor->PlayWorld first.
void Freeze();

// Undo everything Freeze / Step engaged. Safe when nothing is frozen.
void Release();

struct FStepOutcome
{
    double RequestedDeltaSeconds = 0.0;
    // From UWorld::TimeSeconds, which advances only on an unpaused tick
    // (LevelTick.cpp:1607-1611) - so this is the world time the step actually bought,
    // after time dilation and AWorldSettings::FixupDeltaSeconds clamping.
    double WorldSecondsAdvanced = 0.0;
    // Summed from FSlateApplication::OnPostTick, which carries the same delta the UMG
    // animation tick consumed.
    double UiSecondsAdvanced = 0.0;
    // What the level's world settings said a single tick of this length could be, read before
    // the frame ran. Reported so a floored or clamped step is distinguishable from an honoured
    // one without the caller measuring it.
    FWorldStepBudget Budget;
    // The world advanced the requested delta, not a clamped or uncontrolled substitute.
    bool bDeltaHonoured = false;
    bool bTimedOut = false;
};

// Whether a step could start right now. Split out of Step so a handler can refuse through its
// own FHandlerContext instead of through an FAsyncResponseToken: MakeAsyncToken drops the raw
// FResponseCapture pointer, so answering a SYNCHRONOUS refusal through a token would bypass the
// text-formatter and test-fixture capture path (Dispatch/SafePoint.h says the same about
// RunAtSafePoint's inline branch).
bool CanStep(FString& OutErrorCode, FString& OutReason);

// Advance the active PIE session by one frame at DeltaSeconds, then freeze again.
// OnComplete runs on the game thread once the frame has been observed (or the wait gave up).
// Returns false with OutErrorCode / OutReason set when the step cannot start at all - the same
// conditions CanStep reports, re-checked here so the two cannot drift.
bool Step(double DeltaSeconds, TFunction<void(const FStepOutcome&)> OnComplete,
          FString& OutErrorCode, FString& OutReason);

}  // namespace PinWrightPieTime
