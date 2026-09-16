// Copyright (c) 2026 Alexander Penkin. MIT License.

// Implementation of the PIE clock control described in PieTimeControl.h. Read the header
// first - every engine line number behind these calls is cited there.

#include "Handlers/Editor/PieTimeControl.h"

#include "Handlers/CVarPriorityPreservingSet.h"

#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"

namespace PinWrightPieTime
{
namespace Detail
{

// Wall-clock ceiling on the wait for the stepped frame. The frame normally lands on the very
// next core-ticker pass (FEngineLoop::Tick runs GEngine->Tick, then the Slate tick, then the
// core ticker - LaunchEngineLoop.cpp:5859 / :5991 / :6103), so this only fires when the world
// stopped ticking entirely and exists so the RPC always answers.
constexpr double StepWaitTimeoutSeconds = 5.0;

// What a PIE world context's own tick lever held before a step pointed it at the requested
// delta.
struct FPieContextTickSave
{
    bool bSaved = false;
    float FixedTickSeconds = 0.f;
    float AccumulatedTickSeconds = 0.f;
};

struct FState
{
    // --- UI clock (Slate.UseFixedDeltaTime + FSlateApplication::SetFixedDeltaTime) ---
    bool bUiClockEngaged = false;
    bool bSavedSlateUseFixedDelta = false;
    double SavedSlateFixedDelta = 0.0;

    // --- world clock (FApp fixed time step + the PIE context's own tick), engaged only
    //     across a step ---
    bool bWorldClockEngaged = false;
    bool bSavedAppUseFixedTimeStep = false;
    double SavedAppFixedDelta = 0.0;
    // Held by handle, not by pointer: the context outlives nothing in particular and the world
    // can end under an in-flight step.
    FName SteppedContextHandle;
    FPieContextTickSave SavedContextTick;

    // --- motion blur, suppressed for the life of a freeze ---
    bool bMotionBlurSuppressed = false;
    int32 SavedMotionBlurQuality = 0;

    // Widgets whose Slate tick flag we switched off. Restored by recompute, so no saved value.
    TArray<TWeakObjectPtr<UUserWidget>> SuppressedWidgets;

    bool bFrozen = false;
    FDelegateHandle ResumePieHandle;
    FDelegateHandle EndPieHandle;

    // --- in-flight step ---
    bool bStepInFlight = false;
    // Bumped by Release() so an orphaned wait ticker can tell it was superseded.
    uint32 StepId = 0;
    double StepWorldTimeBefore = 0.0;
    FWorldStepBudget StepBudget;
    double StepUiSeconds = 0.0;
    int32 StepUiTicks = 0;
    FDelegateHandle SlatePostTickHandle;
};

inline FState& GetState()
{
    static FState State;
    return State;
}

inline IConsoleVariable* FindSlateFixedDeltaCVar()
{
    return IConsoleManager::Get().FindConsoleVariable(TEXT("Slate.UseFixedDeltaTime"));
}

inline IConsoleVariable* FindMotionBlurQualityCVar()
{
    return IConsoleManager::Get().FindConsoleVariable(TEXT("r.MotionBlurQuality"));
}

// The world context whose tick a step must size: the one the step measures, so the reported
// advance and the lever that produced it can never be about different worlds.
inline FWorldContext* FindSteppedPieContext()
{
    if (!GEngine || !GEditor || !GEditor->PlayWorld)
    {
        return nullptr;
    }
    return GEngine->GetWorldContextFromWorld(GEditor->PlayWorld);
}

inline FPieContextTickSave ApplyContextStepDelta(FWorldContext& Context, double Seconds)
{
    FPieContextTickSave Saved;
    Saved.bSaved = true;
    Saved.FixedTickSeconds = Context.PIEFixedTickSeconds;
    Saved.AccumulatedTickSeconds = Context.PIEAccumulatedTickSeconds;

    Context.PIEFixedTickSeconds = static_cast<float>(Seconds);
    // Drained, because the editor ticks the world once per whole PIEFixedTickSeconds standing
    // in this accumulator (EditorEngine.cpp:2137-2146). A remainder left by a session already
    // running on a fixed PIE FPS would otherwise buy an extra, unrequested tick.
    Context.PIEAccumulatedTickSeconds = 0.f;
    return Saved;
}

inline void RestoreContextStepDelta(FWorldContext& Context, const FPieContextTickSave& Saved)
{
    if (!Saved.bSaved)
    {
        return;
    }
    Context.PIEFixedTickSeconds = Saved.FixedTickSeconds;
    Context.PIEAccumulatedTickSeconds = Saved.AccumulatedTickSeconds;
}

inline void SuppressMotionBlur()
{
    FState& State = GetState();
    if (State.bMotionBlurSuppressed)
    {
        return;
    }
    IConsoleVariable* CVar = FindMotionBlurQualityCVar();
    if (!CVar)
    {
        return;
    }
    State.SavedMotionBlurQuality = CVar->GetInt();
    State.bMotionBlurSuppressed = true;
    CVarPriorityPreservingSet::SetPreservingPriority(CVar, 0);
}

inline void RestoreMotionBlur()
{
    FState& State = GetState();
    if (!State.bMotionBlurSuppressed)
    {
        return;
    }
    if (IConsoleVariable* CVar = FindMotionBlurQualityCVar())
    {
        CVarPriorityPreservingSet::SetPreservingPriority(CVar, State.SavedMotionBlurQuality);
    }
    State.bMotionBlurSuppressed = false;
}

// Switch off the Slate tick flag of every UUserWidget owned by a PIE world. Editor-owned
// widgets (utility widgets, the asset editors) are deliberately out of scope - only the
// session under test is frozen.
inline void SuppressPieWidgetTicks()
{
    FState& State = GetState();
    for (TObjectIterator<UUserWidget> It; It; ++It)
    {
        UUserWidget* Widget = *It;
        if (!IsValid(Widget))
        {
            continue;
        }
        const UWorld* World = Widget->GetWorld();
        if (!World || World->WorldType != EWorldType::PIE)
        {
            continue;
        }
        const TSharedPtr<SWidget> Cached = Widget->GetCachedWidget();
        if (!Cached.IsValid() || !Cached->GetCanTick())
        {
            continue;
        }
        Cached->SetCanTick(false);
        State.SuppressedWidgets.AddUnique(Widget);
    }
}

// UUserWidget::UpdateCanTick recomputes the flag from the widget's own state, so this is exact
// even for a widget that started or ended an animation while frozen.
inline void RestoreWidgetTicks()
{
    FState& State = GetState();
    for (const TWeakObjectPtr<UUserWidget>& Weak : State.SuppressedWidgets)
    {
        if (UUserWidget* Widget = Weak.Get())
        {
            Widget->UpdateCanTick();
        }
    }
    State.SuppressedWidgets.Reset();
}

inline void AttachUiTickProbe()
{
    FState& State = GetState();
    State.StepUiSeconds = 0.0;
    State.StepUiTicks = 0;
    if (State.SlatePostTickHandle.IsValid() || !FSlateApplication::IsInitialized())
    {
        return;
    }
    State.SlatePostTickHandle = FSlateApplication::Get().OnPostTick().AddLambda(
        [](float DeltaSeconds)
        {
            FState& Inner = GetState();
            Inner.StepUiSeconds += DeltaSeconds;
            ++Inner.StepUiTicks;
        });
}

inline void DetachUiTickProbe()
{
    FState& State = GetState();
    if (State.SlatePostTickHandle.IsValid())
    {
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().OnPostTick().Remove(State.SlatePostTickHandle);
        }
        State.SlatePostTickHandle.Reset();
    }
}

// The editor's own Resume / Stop buttons write bDebugPauseExecution directly and never tell
// this module, so without these the widgets we switched off would stay off for the rest of the
// session. FEditorDelegates::ResumePIE fires from UEditorEngine::PlaySessionResumed, EndPIE
// from the teardown; both land on the game thread.
inline void OnEditorReleasedPie(const bool /*bIsSimulating*/)
{
    Release();
}

}  // namespace Detail

bool ValidateStepDelta(double Requested, double& OutDelta, FString& OutReason)
{
    if (Requested == 0.0)
    {
        OutDelta = DefaultStepDeltaSeconds;
        return true;
    }
    if (!FMath::IsFinite(Requested))
    {
        OutReason = TEXT("deltaSeconds must be a finite number");
        return false;
    }
    if (Requested < 0.0)
    {
        OutReason = FString::Printf(TEXT("deltaSeconds must be positive (got %g)"), Requested);
        return false;
    }
    if (Requested > MaxStepDeltaSeconds)
    {
        OutReason = FString::Printf(
            TEXT("deltaSeconds %g exceeds the %g s ceiling on a single step; pass seconds, not "
                 "milliseconds, or issue several steps"),
            Requested, MaxStepDeltaSeconds);
        return false;
    }
    OutDelta = Requested;
    return true;
}

void SetUiClockDelta(double Seconds)
{
    if (!FSlateApplication::IsInitialized())
    {
        return;
    }
    Detail::FState& State = Detail::GetState();
    IConsoleVariable* CVar = Detail::FindSlateFixedDeltaCVar();
    if (!State.bUiClockEngaged)
    {
        State.bSavedSlateUseFixedDelta = CVar && CVar->GetBool();
        State.SavedSlateFixedDelta = FSlateApplication::GetFixedDeltaTime();
        State.bUiClockEngaged = true;
    }
    FSlateApplication::SetFixedDeltaTime(Seconds);
    if (CVar)
    {
        CVarPriorityPreservingSet::SetPreservingPriority(CVar, 1);
    }
}

void RestoreUiClock()
{
    Detail::FState& State = Detail::GetState();
    if (!State.bUiClockEngaged)
    {
        return;
    }
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::SetFixedDeltaTime(State.SavedSlateFixedDelta);
    }
    if (IConsoleVariable* CVar = Detail::FindSlateFixedDeltaCVar())
    {
        CVarPriorityPreservingSet::SetPreservingPriority(CVar, State.bSavedSlateUseFixedDelta ? 1 : 0);
    }
    State.bUiClockEngaged = false;
}

bool IsUiClockEngaged()
{
    return Detail::GetState().bUiClockEngaged;
}

const TCHAR* ClassifyWorldStepClamp(double DilatedSeconds, double AllowedSeconds,
                                    double TimeDilation)
{
    if (AllowedSeconds < DilatedSeconds)
    {
        return TEXT("worldSettings.MaxUndilatedFrameTime");
    }
    if (AllowedSeconds > DilatedSeconds)
    {
        return TEXT("worldSettings.MinUndilatedFrameTime");
    }
    if (!FMath::IsNearlyEqual(TimeDilation, 1.0, UE_DOUBLE_KINDA_SMALL_NUMBER))
    {
        return TEXT("timeDilation");
    }
    return TEXT("none");
}

FWorldStepBudget ResolveWorldStepBudget(AWorldSettings* Settings, double RequestedSeconds)
{
    FWorldStepBudget Budget;
    Budget.ExpectedSeconds = RequestedSeconds;
    if (!Settings)
    {
        return Budget;
    }

    // The same two steps UWorld::Tick applies, in the same order (LevelTick.cpp:1593-1599), and
    // through the level's own FixupDeltaSeconds rather than a copy of its clamp - the function
    // is virtual, so a project can have replaced it.
    Budget.TimeDilation = Settings->GetEffectiveTimeDilation();
    const float Dilated = static_cast<float>(RequestedSeconds * Budget.TimeDilation);
    const float Allowed = Settings->FixupDeltaSeconds(Dilated, static_cast<float>(RequestedSeconds));
    Budget.ExpectedSeconds = Allowed;
    Budget.ClampSource = ClassifyWorldStepClamp(Dilated, Allowed, Budget.TimeDilation);
    return Budget;
}

void EngageWorldClock(double Seconds, FWorldContext* Context)
{
    Detail::FState& State = Detail::GetState();
    const bool bFirstEngage = !State.bWorldClockEngaged;
    if (bFirstEngage)
    {
        State.bSavedAppUseFixedTimeStep = FApp::UseFixedTimeStep();
        State.SavedAppFixedDelta = FApp::GetFixedDeltaTime();
        State.bWorldClockEngaged = true;
    }
    FApp::SetFixedDeltaTime(Seconds);
    FApp::SetUseFixedTimeStep(true);

    // The process lever alone only sizes the ENGINE frame; UEditorEngine::Tick sizes the PIE
    // world tick from the context's own field, so without this the stepped world takes whatever
    // the editor frame took - which is the whole of the defect this fixes.
    if (Context)
    {
        const Detail::FPieContextTickSave Saved = Detail::ApplyContextStepDelta(*Context, Seconds);
        if (bFirstEngage)
        {
            State.SteppedContextHandle = Context->ContextHandle;
            State.SavedContextTick = Saved;
        }
    }
}

void ReleaseWorldClock(FWorldContext* Context)
{
    Detail::FState& State = Detail::GetState();
    if (!State.bWorldClockEngaged)
    {
        return;
    }
    if (Context)
    {
        Detail::RestoreContextStepDelta(*Context, State.SavedContextTick);
    }
    State.SavedContextTick = Detail::FPieContextTickSave();
    State.SteppedContextHandle = NAME_None;

    FApp::SetUseFixedTimeStep(State.bSavedAppUseFixedTimeStep);
    FApp::SetFixedDeltaTime(State.SavedAppFixedDelta);
    State.bWorldClockEngaged = false;
}

void SetWorldClockDelta(double Seconds)
{
    EngageWorldClock(Seconds, Detail::FindSteppedPieContext());
}

void RestoreWorldClock()
{
    // By handle, not by the pointer engaged with: the world can end under an in-flight step, and
    // then there is nothing left to put back but the process clock.
    const FName Handle = Detail::GetState().SteppedContextHandle;
    ReleaseWorldClock(
        (GEngine && !Handle.IsNone()) ? GEngine->GetWorldContextFromHandle(Handle) : nullptr);
}

bool IsWorldClockEngaged()
{
    return Detail::GetState().bWorldClockEngaged;
}

bool IsMotionBlurSuppressed()
{
    return Detail::GetState().bMotionBlurSuppressed;
}

bool IsFrozen()
{
    return Detail::GetState().bFrozen;
}

void Freeze()
{
    Detail::FState& State = Detail::GetState();

    // Zero, not "very small": the UMG animation tick is driven straight from this delta, so a
    // zero-length tick advances no animation while still letting Slate lay out and draw the
    // frame (drawing reads the real delta, which is why the frozen frame still repaints).
    SetUiClockDelta(0.0);
    Detail::SuppressPieWidgetTicks();
    Detail::SuppressMotionBlur();

    if (!State.bFrozen)
    {
        State.bFrozen = true;
        State.ResumePieHandle = FEditorDelegates::ResumePIE.AddStatic(&Detail::OnEditorReleasedPie);
        State.EndPieHandle = FEditorDelegates::EndPIE.AddStatic(&Detail::OnEditorReleasedPie);
    }
}

void Release()
{
    Detail::FState& State = Detail::GetState();

    // Orphan any wait ticker still in flight before touching the clocks it would restore.
    ++State.StepId;
    State.bStepInFlight = false;

    Detail::DetachUiTickProbe();
    Detail::RestoreWidgetTicks();
    Detail::RestoreMotionBlur();
    RestoreUiClock();
    RestoreWorldClock();

    if (State.bFrozen)
    {
        FEditorDelegates::ResumePIE.Remove(State.ResumePieHandle);
        FEditorDelegates::EndPIE.Remove(State.EndPieHandle);
        State.ResumePieHandle.Reset();
        State.EndPieHandle.Reset();
        State.bFrozen = false;
    }
}

bool CanStep(FString& OutErrorCode, FString& OutReason)
{
    if (!GEditor || !GEditor->PlayWorld)
    {
        OutErrorCode = TEXT("NO_ACTIVE_SESSION");
        OutReason = TEXT("No active PIE session to step");
        return false;
    }
    if (Detail::GetState().bStepInFlight)
    {
        OutErrorCode = TEXT("STEP_IN_PROGRESS");
        OutReason = TEXT("A step is already in flight; wait for it to answer before issuing another");
        return false;
    }
    return true;
}

bool Step(double DeltaSeconds, TFunction<void(const FStepOutcome&)> OnComplete,
          FString& OutErrorCode, FString& OutReason)
{
    if (!CanStep(OutErrorCode, OutReason))
    {
        return false;
    }

    UWorld* PlayWorld = GEditor->PlayWorld;
    Detail::FState& State = Detail::GetState();

    // Both UI clocks move to the step delta, and the widget Tick flags come back on, BEFORE the
    // world is let go - the stepped frame must be the first one that sees any of it.
    Detail::RestoreWidgetTicks();
    SetUiClockDelta(DeltaSeconds);
    SetWorldClockDelta(DeltaSeconds);

    State.StepWorldTimeBefore = PlayWorld->GetTimeSeconds();
    // Unchecked: a world without settings must report an unclamped budget, not assert.
    State.StepBudget =
        ResolveWorldStepBudget(PlayWorld->GetWorldSettings(/*bCheckStreamingPersistent*/ false,
                                                           /*bChecked*/ false),
                               DeltaSeconds);
    Detail::AttachUiTickProbe();

    PlayWorld->bDebugFrameStepExecution = true;
    PlayWorld->bDebugPauseExecution = false;

    const uint32 StepId = ++State.StepId;
    State.bStepInFlight = true;

    const double Deadline = FPlatformTime::Seconds() + Detail::StepWaitTimeoutSeconds;
    const TWeakObjectPtr<UWorld> WeakWorld(PlayWorld);

    // One core-ticker hop puts us past the whole frame: FEngineLoop::Tick runs the world, then
    // Slate, then the core ticker, and a ticker added during one pass first fires on the next.
    // The wait is nonetheless expressed as a CONDITION, not a frame count, so a frame the editor
    // skipped (throttled, idle, a modal) only costs another pass instead of answering with
    // numbers no frame produced.
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [StepId, WeakWorld, DeltaSeconds, Deadline, OnComplete](float) -> bool
        {
            Detail::FState& Inner = Detail::GetState();
            if (Inner.StepId != StepId || !Inner.bStepInFlight)
            {
                return false;  // released or superseded; the releasing path already restored
            }

            UWorld* World = WeakWorld.Get();
            // UWorld::Tick re-arms bDebugPauseExecution at the end of the stepped tick
            // (LevelTick.cpp:1956-1961), which is the world's own receipt that the frame ran.
            const bool bWorldStepped = !World || World->bDebugPauseExecution;
            // Without Slate there is no probe to fire and no UI to step, so the UI half is
            // vacuously done - otherwise a headless caller would wait out the whole deadline and
            // be told the step timed out when the world frame ran fine.
            const bool bUiStepped = Inner.StepUiTicks > 0 || !FSlateApplication::IsInitialized();
            const bool bComplete = bWorldStepped && bUiStepped;
            if (!bComplete && FPlatformTime::Seconds() < Deadline)
            {
                return true;
            }

            FStepOutcome Outcome;
            Outcome.RequestedDeltaSeconds = DeltaSeconds;
            Outcome.WorldSecondsAdvanced =
                World ? (World->GetTimeSeconds() - Inner.StepWorldTimeBefore) : 0.0;
            Outcome.UiSecondsAdvanced = Inner.StepUiSeconds;
            Outcome.Budget = Inner.StepBudget;
            // UWorld::TimeSeconds is a float accumulator, so the difference of two readings
            // quantizes as the session gets long; the tolerance is that noise floor, not a
            // licence for a clamped step to pass as honoured.
            Outcome.bDeltaHonoured = bComplete
                && FMath::Abs(Outcome.WorldSecondsAdvanced - DeltaSeconds)
                       <= FMath::Max(1.0e-4, DeltaSeconds * 0.02);
            Outcome.bTimedOut = !bComplete;

            Detail::DetachUiTickProbe();
            RestoreWorldClock();
            Inner.bStepInFlight = false;
            if (World)
            {
                // "and pause again" - normally already true from the re-arm above, but a timed-out
                // step must not leave the session running.
                World->bDebugPauseExecution = true;
                Freeze();
            }
            else
            {
                // The session ended under the step. Nothing is left to freeze, and holding the
                // process-global Slate clock at zero with no PIE world would stall the editor's
                // own UI animations for the rest of the session.
                Release();
            }

            OnComplete(Outcome);
            return false;
        }),
        0.0f);

    return true;
}

}  // namespace PinWrightPieTime
