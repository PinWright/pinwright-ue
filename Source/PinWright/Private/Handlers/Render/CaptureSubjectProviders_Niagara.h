// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EditorViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "PreviewScene.h"
#include "UObject/UObjectIterator.h"

#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CaptureSubject.h"

// The Niagara half of the capture-subject resolver, kept OUT of the provider .cpp so it can be
// tested without opening an asset editor. CaptureSubjectProviders_Niagara.cpp is the wiring that
// registers these as the ESubjectKind::Niagara provider; everything that can be asserted about a
// particle subject lives here.
//
// The viewport walk, the allow-list, the toolkit-name gate and the three-state close rule are NOT
// here - they are C1's, shared by every kind (CaptureSubject.h §2.5). This file adds only what is
// Niagara: which component to drive, how to put it at an instant, how to bound it, and what the
// response is allowed to say about the result.
//
// WHAT IS TRUE ABOUT PARTICLE CAPTURE, AND WHAT IS NOT.
// Nothing here promises reproducibility, and no function emits a `reproducible` field in any
// spelling. Niagara determinism exists at three scopes and all three default OFF:
// UNiagaraSystem::bDeterminism + RandomSeed (NiagaraSystem.h:1073-1085),
// FVersionedNiagaraEmitterData::bDeterminism + RandomSeed (NiagaraEmitter.h:310-316), and
// UNiagaraComponent::RandomSeedOffset (NiagaraComponent.h:96-103). With system determinism off the
// per-instance seed is literally FMath::Rand() on every reset (NiagaraSystemInstance.cpp:894), no
// determinism knob covers GPU emitters at all, and the emitter-scope comment scopes its own
// guarantee to "as long as delta time is not variable". So the response reports WHAT WAS DONE -
// the tick delta, the tick count, the age the simulation actually reached, and which of the three
// scopes is off - and promises nothing.
//
// "Defaults off" is the CLASS default and not what an old asset loads with. UNiagaraSystem::Serialize
// forces bDeterminism = true on any system package saved before
// FNiagaraCustomVersion::ChangeSystemDeterministicDefault (NiagaraSystem.cpp:1243-1249), because
// that was the older default - so a stock template system can come off disk deterministic. Every
// scope here is therefore MEASURED off the asset, never assumed from the default; ReadDeterminism
// exists for exactly that reason.
namespace PinWrightCaptureSubjectNiagara
{
    // Fixed simulation step for every time set. Explicit and echoed in the response because a
    // particle frame is only interpretable next to the delta that produced it, and because the
    // emitter-scope determinism guarantee is void under a variable delta (NiagaraEmitter.h:312).
    // 1/30 s matches Niagara's own default SeekDelta.
    inline constexpr float DefaultTickDeltaSeconds = 1.0f / 30.0f;

    // The time window a Niagara subject offers when the caller names none.
    //
    // This is a STATED DEFAULT, not a measurement, and it is the one number here that could not be
    // derived from the asset: a Niagara system carries no authored duration. UNiagaraSystem has a
    // warmup time and an optional fixed tick delta, but nothing that says when the effect is over -
    // emitters loop, run once, or run forever, per emitter. Two seconds covers the visible life of
    // a typical burst at the default 1/30 delta (60 ticks). A caller that knows better passes its
    // own instants; the window is published so it is never implicit.
    inline constexpr float DefaultPreviewDurationSeconds = 2.0f;

    // FNiagaraSystemToolkit::GetToolkitFName() returns FName("Niagara")
    // (NiagaraSystemToolkit.cpp:351-354), and FAssetEditorToolkit::GetEditorName() returns
    // GetToolkitFName() (AssetEditorToolkit.cpp:502-505). This is the name handed to
    // PinWrightCaptureSubject::AcquireAssetEditorViewport, which gates the FAssetEditorToolkit
    // downcast on it; it is already in C1's GetSupportedAssetEditorToolkitNames().
    inline FName NiagaraSystemToolkitName()
    {
        return FName(TEXT("Niagara"));
    }

    inline bool IsNiagaraSystemEditor(FName EditorName)
    {
        return EditorName == NiagaraSystemToolkitName();
    }

    // The preview component, found through the viewport client's own preview world - the same
    // route the Persona path takes (AnimationPreviewCaptureHandler.cpp:160-193), with
    // UNiagaraComponent substituted for UDebugSkelMeshComponent.
    //
    // FNiagaraSystemViewModel::GetPreviewComponent() is public and exported
    // (NiagaraSystemViewModel.h:217) and is NOT used: it can hand back a component belonging to a
    // cached data-processing view model rather than the world the viewport actually draws, and a
    // frame photographed of a different component than the one advanced is the exact failure this
    // whole layer exists to prevent.
    //
    // OutCandidateCount reports how many matched, for the same reason Persona's does: a host that
    // puts a second Niagara component in the preview world would otherwise silently change which
    // system got photographed.
    inline UNiagaraComponent* FindPreviewNiagaraComponent(
        FEditorViewportClient& ViewportClient, UNiagaraSystem* PreferredSystem, int32& OutCandidateCount)
    {
        OutCandidateCount = 0;
        FPreviewScene* PreviewScene = ViewportClient.GetPreviewScene();
        UWorld* PreviewWorld = PreviewScene ? PreviewScene->GetWorld() : nullptr;
        if (!PreviewWorld)
        {
            return nullptr;
        }

        UNiagaraComponent* FirstMatch = nullptr;
        UNiagaraComponent* PreferredMatch = nullptr;
        for (TObjectIterator<UNiagaraComponent> It; It; ++It)
        {
            UNiagaraComponent* Component = *It;
            // TObjectIterator also visits class default objects, and every other Niagara editor
            // open in this process has its own preview component in its own preview world.
            if (!Component || Component->IsTemplate() || Component->GetWorld() != PreviewWorld)
            {
                continue;
            }
            ++OutCandidateCount;
            if (!FirstMatch)
            {
                FirstMatch = Component;
            }
            if (PreferredSystem && !PreferredMatch && Component->GetAsset() == PreferredSystem)
            {
                PreferredMatch = Component;
            }
        }
        return PreferredMatch ? PreferredMatch : FirstMatch;
    }

    // ---- Component state, saved so the artist's tab survives the capture ---------------------

    // Everything the time setter and the bounds pin change on the preview component. A review
    // verb that leaves a Niagara tab frozen at 0.4 s with a pinned bounding box is not read-only
    // in any sense that matters.
    struct FComponentState
    {
        bool bCaptured = false;
        ENiagaraAgeUpdateMode AgeUpdateMode = ENiagaraAgeUpdateMode::TickDeltaTime;
        float DesiredAge = 0.0f;
        float SeekDelta = 0.0f;
        FBox SystemFixedBounds = FBox(ForceInit);
        bool bFixedBoundsWereSet = false;
        // Both changed by EnsureSystemInstance and both restored, for the same reason the age mode
        // is: a review verb that leaves the artist's tab in force-solo with a blocking compile wait
        // has changed how their editor behaves after the capture ended.
        bool bWaitForCompilationOnActivate = false;
        bool bForceSolo = false;
        // Changed by the subject-coverage differential, which hides the component to draw a
        // reference frame. Restored here rather than only in the measurement loop because a
        // capture that fails between the hide and the show would otherwise leave the artist
        // looking at an empty preview tab and no reason for it - the single worst state this
        // provider could leave behind, since an invisible effect is indistinguishable from a
        // broken one.
        bool bVisible = true;
    };

    inline void CaptureComponentState(const UNiagaraComponent& Component, FComponentState& OutState)
    {
        OutState.bCaptured = true;
        OutState.AgeUpdateMode = Component.GetAgeUpdateMode();
        OutState.DesiredAge = Component.GetDesiredAge();
        OutState.SeekDelta = Component.GetSeekDelta();
        OutState.SystemFixedBounds = Component.GetSystemFixedBounds();
        OutState.bFixedBoundsWereSet = OutState.SystemFixedBounds.IsValid != 0;
        OutState.bWaitForCompilationOnActivate = Component.bWaitForCompilationOnActivate != 0;
        OutState.bForceSolo = Component.GetForceSolo();
        // GetVisibleFlag, not IsVisible(): IsVisible() folds in the parent chain and the owner's
        // hidden state, so restoring from it would WRITE a value that was never on this component.
        OutState.bVisible = Component.GetVisibleFlag();
    }

    // Show or hide the preview component without touching anything else. The subject-coverage
    // differential in PoseListCapture drives this twice per shot - once to draw a reference frame
    // with the subject gone, once to put it back.
    //
    // PROPAGATES TO CHILDREN (bPropagateToChildren=true). A Niagara system's renderers hang off
    // the component, and a hide that stopped at the component itself would leave the very pixels
    // the differential is trying to remove still on screen - the measurement would then read ~0
    // for a perfectly good frame and warn about a subject that is plainly there.
    inline void SetPreviewComponentVisible(UNiagaraComponent& Component, bool bVisible)
    {
        Component.SetVisibility(bVisible, /*bPropagateToChildren=*/true);
    }

    // ---- THE INSTANCE HAS TO EXIST BEFORE ANYTHING CAN BE SIMULATED OR MEASURED -------------
    //
    // THE DEFECT THIS CLOSES, measured rather than reasoned about. Every capture of
    // /Niagara/DefaultAssets/Templates/Systems/SimpleExplosion reported
    // `subject.boundsRadius = 173.2050807568877` -- which is exactly 100*sqrt(3), the sphere radius
    // of the system's AUTHORED FBox(-100..100) -- while `ageMeasured` came back false and some
    // frames contained no particles at all. All three symptoms are one cause:
    //
    //   UNiagaraComponent::AdvanceSimulation is a SILENT NO-OP when SystemInstanceController is
    //   invalid (NiagaraComponent.cpp:1098-1104 -- `if (SystemInstanceController.IsValid() && ...)`
    //   with no else), and ActivateSystem returns WITHOUT creating one whenever the asset has
    //   outstanding compilation requests, the world is not ready, or the PSO cache is not warm
    //   (:1366-1375 and :1505-1512, both setting bAwaitingActivationDueToNotReady and returning).
    //
    // A freshly opened Niagara editor is exactly that state. So the whole chain ran and reported
    // success while doing nothing: the advance ticked zero times, TryReadSimulatedAge found no
    // controller, all five bounds probes were skipped, MeasureAndPinBounds fell through to
    // System->GetFixedBounds(), and the camera was solved from a box the effect does not fill.
    //
    // WHAT THIS DOES ABOUT IT. bWaitForCompilationOnActivate makes ActivateSystem block on the
    // compile instead of deferring (:1357-1362), and force-solo makes the component own its own
    // tick rather than waiting for a batched world tick that a preview world may never run. The
    // PSO branch is not fixable from here at all -- it clears when the render thread catches up --
    // so the loop flushes rendering commands and retries a BOUNDED number of times.
    //
    // RETURNS WHAT HAPPENED. False means the instance never came up, and every caller treats that
    // as "this frame is not of the moment it claims" rather than capturing anyway. Silence here is
    // what produced a pure-backdrop PNG reporting boundsInFrame:true.
    inline bool EnsureSystemInstance(UNiagaraComponent& Component, int32 MaxAttempts = 4)
    {
        Component.bWaitForCompilationOnActivate = 1;
        Component.SetForceSolo(true);
        for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
        {
            Component.Activate(/*bReset=*/true);
            FNiagaraSystemInstanceControllerConstPtr Controller = Component.GetSystemInstanceController();
            if (Controller.IsValid() && Controller->IsValid())
            {
                return true;
            }
            // The PSO-cache branch clears only once the render thread has caught up, so give it a
            // chance between attempts. Bounded, and on the game thread by contract - this runs
            // inside Acquire, which is already a game-thread call.
            FlushRenderingCommands();
        }
        FNiagaraSystemInstanceControllerConstPtr Controller = Component.GetSystemInstanceController();
        return Controller.IsValid() && Controller->IsValid();
    }

    inline void RestoreComponentState(UNiagaraComponent& Component, const FComponentState& State)
    {
        if (!State.bCaptured)
        {
            return;
        }
        if (State.bFixedBoundsWereSet)
        {
            Component.SetSystemFixedBounds(State.SystemFixedBounds);
        }
        else
        {
            Component.ClearSystemFixedBounds();
        }
        Component.SetSeekDelta(State.SeekDelta);
        Component.SetDesiredAge(State.DesiredAge);
        Component.SetAgeUpdateMode(State.AgeUpdateMode);
        Component.bWaitForCompilationOnActivate = State.bWaitForCompilationOnActivate ? 1 : 0;
        Component.SetForceSolo(State.bForceSolo);
        SetPreviewComponentVisible(Component, State.bVisible);
        // Put the simulation back on its own clock. Without this the tab stays frozen at whatever
        // instant the last shot asked for, which looks exactly like a broken effect.
        Component.ResetSystem();
    }

    // ---- Time ------------------------------------------------------------------------------

    // What a single time set actually did, measured rather than echoed.
    struct FTimeStep
    {
        double RequestedSeconds = 0.0;
        float TickDeltaSeconds = DefaultTickDeltaSeconds;
        int32 TickCount = 0;
        // The age the simulation reached, read off the system instance after the ticks ran - not
        // the number that was passed in. bAgeMeasured is false when no instance existed to ask.
        double AchievedAgeSeconds = 0.0;
        bool bAgeMeasured = false;
        // The same reading taken BEFORE the ticks ran, which is what turns the one above from a
        // number into evidence. AdvanceToTime resets first, so a healthy drive starts at 0; a
        // non-zero here means the reset did not take and the achieved age is NOT TickCount ticks
        // from the system's start.
        double StartAgeSeconds = 0.0;
        bool bStartAgeMeasured = false;
        // THE TICKS REACHED THE SIMULATION - derived from the two ages above, never from the
        // existence of a system instance. FALSE MEANS THE FRAME IS NOT OF THE MOMENT IT CLAIMS.
        //
        // This was once the return of EnsureSystemInstance, i.e. "a controller exists". That is a
        // necessary condition and not a sufficient one, and the gap is not academic - it is the
        // whole of board ticket B-niagara-preview-age-zero-after-ticks. With a perfectly valid
        // controller the advance still ticks nothing when the instance is paused or complete
        // (NiagaraSystemInstance.cpp:989, 993-996); the system simulation still runs zero
        // sub-ticks under a system-authored fixed tick delta (NiagaraSystemSimulation.cpp:
        // 1053-1080) or the skip-tick cvar (:1149-1152); and Tick_GameThread still returns before
        // `Age += DeltaSeconds` when the attached component is gone, when the instance completes
        // mid-run, or when dirty data interfaces force a Reset(ReInit) that zeroes the age again
        // (NiagaraSystemInstance.cpp:2597-2627). Every one of those published `simulated: true`
        // beside `achievedAgeSeconds: 0` - a response asserting the ticks ran and measuring, in
        // the same object, that they had not.
        //
        // TickCount 0 is the one case with no age delta to measure: the instant asked for IS the
        // system's start, so a live instance is the whole of the claim there.
        bool bSimulated = false;
        // Why the ticks did not reach the simulation, READ off the instance afterwards rather than
        // guessed. Empty whenever bSimulated is true. See DescribeAdvanceStall.
        FString StallReason;
    };

    // The age the simulation is actually at, read through the component's system instance.
    // FNiagaraSystemInstanceController's GetAge shim and FNiagaraSystemInstance::GetAge are both
    // header-inline (NiagaraSystemInstanceController.h:31, 129; NiagaraSystemInstance.h:377), so
    // this needs no exported symbol even though UNiagaraComponent is MinimalAPI.
    inline bool TryReadSimulatedAge(const UNiagaraComponent& Component, double& OutAgeSeconds)
    {
        FNiagaraSystemInstanceControllerConstPtr Controller = Component.GetSystemInstanceController();
        if (!Controller.IsValid() || !Controller->IsValid())
        {
            return false;
        }
        OutAgeSeconds = static_cast<double>(Controller->GetAge());
        return true;
    }

    // WHY AN ADVANCE THAT ASKED FOR TICKS LEFT THE AGE WHERE IT WAS. Every branch is READ off the
    // instance after the fact; none of them is inferred from the request.
    //
    // The first is the one this provider already knew about - no controller, so
    // UNiagaraComponent::AdvanceSimulation is an `if` with no else (NiagaraComponent.cpp:
    // 1098-1104). The rest are the ones a controller-validity check cannot see, and they are why
    // `simulated` could report true beside `achievedAgeSeconds: 0`: the controller is valid in
    // every one of them.
    //
    // Ordered the way the engine reaches them - the two states AdvanceSimulation tests before it
    // ticks at all, then the two throttles that swallow each individual tick, then the paths
    // inside the instance tick itself, which cannot be told apart from here and are named
    // together rather than guessed between.
    inline FString DescribeAdvanceStall(const UNiagaraComponent& Component, float TickDeltaSeconds)
    {
        FNiagaraSystemInstanceControllerConstPtr Controller = Component.GetSystemInstanceController();
        if (!Controller.IsValid() || !Controller->IsValid())
        {
            return TEXT(
                "No Niagara system instance controller existed when the ticks were issued, so "
                "UNiagaraComponent::AdvanceSimulation returned without doing anything "
                "(NiagaraComponent.cpp:1098-1104). The frames show whatever the preview was already "
                "displaying, at whatever age it was already at.");
        }
        if (Controller->IsPaused())
        {
            return TEXT(
                "The Niagara system instance is paused, and FNiagaraSystemInstance::AdvanceSimulation "
                "returns without ticking while paused (NiagaraSystemInstance.cpp:989).");
        }
        if (Controller->IsComplete())
        {
            return TEXT(
                "The Niagara system instance reports Complete (which also covers Disabled, "
                "NiagaraSystemInstance.h:266-267), and FNiagaraSystemInstance::AdvanceSimulation "
                "returns without ticking in that state (NiagaraSystemInstance.cpp:993-996). Every "
                "enabled emitter has finished or failed to initialise, so there is nothing left to "
                "simulate at any instant - not just at this one.");
        }
        // A process-wide debug throttle, checked because it is invisible from the response
        // otherwise and because it freezes EVERY Niagara capture in the process at age 0 while
        // leaving every instance valid, active and unpaused - the hardest shape of this defect to
        // attribute from a response alone.
        const TCHAR* const SkipTickCVarName = TEXT("fx.Niagara.SystemSimulation.SkipTickDeltaSeconds");
        if (const IConsoleVariable* SkipTickCVar =
                IConsoleManager::Get().FindConsoleVariable(SkipTickCVarName))
        {
            const float SkipAtOrBelow = SkipTickCVar->GetFloat();
            if (SkipAtOrBelow > 0.0f && TickDeltaSeconds <= SkipAtOrBelow)
            {
                return FString::Printf(TEXT(
                    "%s is set to %g s, and FNiagaraSystemSimulation::Tick_GameThread_Internal "
                    "returns without simulating anything for any tick at or below it "
                    "(NiagaraSystemSimulation.cpp:1149-1152). The %g s tick delta this capture used "
                    "is inside that window, so none of the ticks ran. Set the variable to 0 to "
                    "capture particles."),
                    SkipTickCVarName, static_cast<double>(SkipAtOrBelow),
                    static_cast<double>(TickDeltaSeconds));
            }
        }
        if (const UNiagaraSystem* System = Component.GetAsset())
        {
            if (System->HasFixedTickDelta())
            {
                return FString::Printf(TEXT(
                    "The system overrides the tick delta (FixedTickDeltaTime = %g s), so it does not "
                    "consume the %g s delta it was handed: FNiagaraSystemSimulation::Tick_GameThread "
                    "turns each requested tick into a time budget and runs whole sub-ticks of ITS "
                    "delta instead - and none at all when that delta is 0 "
                    "(NiagaraSystemSimulation.cpp:1053-1080). Capture this system at a tick delta at "
                    "or above its own, or clear bFixedTickDelta."),
                    static_cast<double>(System->GetFixedTickDeltaTime()),
                    static_cast<double>(TickDeltaSeconds));
            }
        }
        return TEXT(
            "The ticks reached a live, running Niagara system instance and its age did not move. "
            "FNiagaraSystemInstance::Tick_GameThread returns before `Age += DeltaSeconds` when the "
            "attached component has gone away, when the instance completes mid-run, or when dirty "
            "data interfaces force a Reset(ReInit) that zeroes the age again "
            "(NiagaraSystemInstance.cpp:2597-2627). These three are indistinguishable from outside "
            "the instance; check the system's data interfaces and emitter compile state.");
    }

    // How many whole fixed ticks fit in a requested instant.
    //
    // Floor, matching UNiagaraComponent::AdvanceSimulationByTime (NiagaraComponent.cpp:1106-1112),
    // so the two spellings of the same request land on the same frame and no shot is ever
    // simulated PAST the instant it asked for. The remainder is published rather than absorbed -
    // see achievedAgeSeconds in the response block.
    //
    // Floored against the delta the caller MEANT, not against the float they were able to pass. A
    // float cannot hold 1/30: the nearest is 0.0333333350718021, LARGER than 1/30 by a relative
    // 5.2e-8. Dividing an exact multiple by it therefore lands just below the integer -
    // 1.0 / that is 29.999998435 - and a plain FloorToInt drops a whole tick: 29 ticks for one
    // second, 59 for two, 119 for two at half the delta. The shortfall is RELATIVE, so it grows
    // with the count and no constant offset can absorb it; only snapping at the same relative
    // scale can.
    //
    // The snap is sized by the representation error and nothing else. A float carries at most a
    // half-ULP relative error (2^-24) against the value it stands for, so FLT_EPSILON (2^-23) is a
    // full ULP of headroom over that bound while staying ~10^5 times tighter than the half tick
    // that would change which frame is chosen: at t = 2 s a request must sit within ~2e-7 s of a
    // tick boundary to snap to it, which is to say be indistinguishable from it in a float. A
    // genuine non-multiple - 1.05 s at 1/30, quotient 31.4999983 - is nowhere near and still
    // floors to 31.
    inline int32 TickCountForTime(double TimeSeconds, float TickDeltaSeconds)
    {
        if (!(TickDeltaSeconds > UE_SMALL_NUMBER) || !(TimeSeconds > 0.0))
        {
            return 0;
        }
        const double Quotient = TimeSeconds / static_cast<double>(TickDeltaSeconds);
        const double Nearest = FMath::RoundToDouble(Quotient);
        const double SnapTolerance = FMath::Max(Nearest, 1.0) * static_cast<double>(FLT_EPSILON);
        if (Nearest >= 0.0 && FMath::Abs(Quotient - Nearest) <= SnapTolerance)
        {
            return static_cast<int32>(Nearest);
        }
        return FMath::FloorToInt(Quotient);
    }

    // Drive the system to one instant: reset to age 0, then run TickCount fixed steps.
    //
    // WHY AdvanceSimulation AND NOT SeekToDesiredAge. AdvanceSimulation forces solo mode and runs
    // ManualTick N times on the game thread, completing before it returns
    // (NiagaraSystemInstance.cpp:987-1010). SeekToDesiredAge routes the jump through
    // TickComponent's seek branch, whose MultiTick is throttled by MaxSimTime - default 33 ms
    // (NiagaraComponent.cpp:651) - so a long seek is spread across frames the capture never gives
    // it and the frame is photographed mid-seek.
    //
    // WHY FROM A RESET. The instant is ABSOLUTE. Advancing from wherever the last shot left the
    // system would make shot N's content depend on the order the poses happened to be listed in.
    //
    // WHY IT ENDS IN DesiredAge MODE. Between this call and the readback the preview world ticks
    // the component several times, which under TickDeltaTime would carry the simulation past the
    // instant that was asked for. With AgeUpdateMode DesiredAge and DesiredAge equal to the age
    // the simulation actually reached, TickComponent's AgeDiff falls under KINDA_SMALL_NUMBER and
    // it ticks nothing (NiagaraComponent.cpp:995-1000). The frame is held, not merely arrived at.
    inline bool AdvanceToTime(UNiagaraComponent& Component, double TimeSeconds, float TickDeltaSeconds,
        FTimeStep& OutStep, FString& OutErrorCode, FString& OutErrorMessage)
    {
        OutStep = FTimeStep();
        OutStep.RequestedSeconds = TimeSeconds;
        OutStep.TickDeltaSeconds = TickDeltaSeconds;

        if (!(TickDeltaSeconds > UE_SMALL_NUMBER))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(
                TEXT("Niagara tick delta must be greater than %g seconds (got %g). ")
                TEXT("UNiagaraComponent::AdvanceSimulation silently does nothing at or below it ")
                TEXT("(NiagaraComponent.cpp:1100), so a smaller delta would report ticks that never ran."),
                static_cast<double>(UE_SMALL_NUMBER), static_cast<double>(TickDeltaSeconds));
            return false;
        }
        if (TimeSeconds < 0.0)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(
                TEXT("Niagara subject time must be at or after the system's start (got %g seconds). ")
                TEXT("A particle system has no state before age 0 to simulate back to."),
                TimeSeconds);
            return false;
        }

        OutStep.TickCount = TickCountForTime(TimeSeconds, TickDeltaSeconds);

        Component.SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
        Component.SetSeekDelta(TickDeltaSeconds);
        // Activate(true) under the hood (NiagaraComponent.cpp:1077-1080): resets the instance to
        // age 0 and clears any pause state, which AdvanceSimulation requires - it early-returns
        // while paused (NiagaraSystemInstance.cpp:989).
        //
        // Routed through EnsureSystemInstance rather than a bare ResetSystem() because a reset that
        // leaves no controller behind makes every line below a no-op that still reports success.
        //
        // ITS RETURN IS NOT THE ANSWER TO "DID THE TICKS RUN". It proves a controller exists, which
        // is necessary and not sufficient - see FTimeStep::bSimulated for the five engine paths on
        // which the advance ticks nothing with a valid controller in hand. So the age is read on
        // BOTH sides of the advance and the verdict is derived from the second reading, the way
        // docs/rpc-design.md section 4 requires: the write path here is AdvanceSimulation, and it
        // cannot fake a number it never wrote.
        const bool bInstanceLive = EnsureSystemInstance(Component);
        OutStep.bStartAgeMeasured = TryReadSimulatedAge(Component, OutStep.StartAgeSeconds);
        if (bInstanceLive && OutStep.TickCount > 0)
        {
            Component.AdvanceSimulation(OutStep.TickCount, TickDeltaSeconds);
        }

        OutStep.bAgeMeasured = TryReadSimulatedAge(Component, OutStep.AchievedAgeSeconds);
        OutStep.bSimulated = OutStep.TickCount == 0
            ? bInstanceLive
            : bInstanceLive && OutStep.bStartAgeMeasured && OutStep.bAgeMeasured &&
                  OutStep.AchievedAgeSeconds > OutStep.StartAgeSeconds;
        if (!OutStep.bSimulated)
        {
            OutStep.StallReason = DescribeAdvanceStall(Component, TickDeltaSeconds);
        }

        const float HoldAge = OutStep.bAgeMeasured
            ? static_cast<float>(OutStep.AchievedAgeSeconds)
            : static_cast<float>(OutStep.TickCount * static_cast<double>(TickDeltaSeconds));
        Component.SetDesiredAge(HoldAge);
        Component.SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
        return true;
    }

    // ---- Bounds ----------------------------------------------------------------------------

    struct FBoundsPlan
    {
        // Component-local box, the shape SetSystemFixedBounds takes.
        FBox LocalBounds = FBox(ForceInit);
        FVector WorldOrigin = FVector::ZeroVector;
        double WorldRadius = 0.0;
        // Which box was pinned, and the two answers are NOT interchangeable:
        //
        //   "pinnedFixedBounds"   - the union of the system's live local bounds over the probe
        //                           instants. A measurement of this effect.
        //   "authoredFixedBounds" - the asset's own FixedBounds, used because not one probe
        //                           produced a running simulation to measure. A property of the
        //                           asset, unrelated to what the effect fills at any instant.
        //
        // They were once the same string, and that is exactly how the AdvanceSimulation no-op hid
        // for so long: every capture of SimpleExplosion reported boundsRadius 173.2050807568877 -
        // 100*sqrt(3), the authored box - under a source name that claimed it had been measured.
        // A caller had no field anywhere in the response that could tell the two apart.
        //
        // Empty when nothing could be pinned at all.
        FString BoundsSource;
        // Non-empty when the numbers above cannot be trusted for framing. Published through
        // FResolvedSubject::BoundsWarning, never swallowed: a wrong frame that looks confident is
        // worse than a warned one.
        FString BoundsWarning;
        TArray<FString> GpuEmittersWithoutFixedBounds;
        bool bPinned = false;
        // Probe instants asked for, and how many of them the ticks actually moved (FTimeStep::
        // bSimulated, which is a measured age delta). A shortfall here is the direct, greppable
        // signature of the AdvanceSimulation no-op: whatever box was pinned came from the asset
        // rather than from the effect. 0 means no system instance came up at all; the first probe
        // sits at t=0 and has no ticks to move, so a live-but-frozen system scores exactly 1.
        int32 ProbesAttempted = 0;
        int32 ProbesSimulated = 0;
    };

    // GPU emitters never get dynamic bounds. FNiagaraEmitterInstanceImpl::CalculateBounds falls
    // back to FVersionedNiagaraEmitterData::GetDefaultFixedBounds() - a hardcoded
    // FBox(FVector(-100), FVector(100)) (NiagaraEmitter.h:449) - for any GPU emitter whose
    // CalculateBoundsMode is Dynamic (NiagaraEmitterInstanceImpl.cpp:974-980). A Programmable GPU
    // emitter that never sets its bounds from script gets no box at all. Either way the measured
    // extent is unrelated to what the effect actually fills, so auto-framing is wrong and the
    // response has to say so rather than silently mis-frame.
    //
    // Only CalculateBoundsMode == Fixed is exempt: that path reads EmitterData->FixedBounds,
    // which is authored (NiagaraEmitterInstanceImpl.cpp:968-971).
    inline FString MakeGpuBoundsWarning(const UNiagaraSystem& System, TArray<FString>& OutEmitterNames)
    {
        OutEmitterNames.Reset();
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            if (!Handle.GetIsEnabled())
            {
                continue;
            }
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            if (!EmitterData || EmitterData->SimTarget != ENiagaraSimTarget::GPUComputeSim)
            {
                continue;
            }
            if (EmitterData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Fixed)
            {
                continue;
            }
            OutEmitterNames.Add(Handle.GetName().ToString());
        }
        if (OutEmitterNames.Num() == 0)
        {
            return FString();
        }
        return FString::Printf(
            TEXT("Bounds are not measurable for GPU emitter%s %s: a GPU emitter without authored ")
            TEXT("fixed bounds reports a hardcoded 200 cm cube (NiagaraEmitter.h:449) regardless of ")
            TEXT("its real extent, so any camera solved from these bounds may miss the effect. ")
            TEXT("Set CalculateBoundsMode to Fixed on the emitter, or pass an explicit camera."),
            OutEmitterNames.Num() == 1 ? TEXT("") : TEXT("s"),
            *FString::Join(OutEmitterNames, TEXT(", ")));
    }

    // Measure a box across the instants that will actually be captured, then PIN it.
    //
    // WHY PIN AT ALL. UNiagaraComponent::CalcBounds prefers the live CurrLocalBounds over
    // SystemFixedBounds (NiagaraComponent.cpp:2433-2481), and CurrLocalBounds is refreshed only
    // when the new box escapes the old one or every 5 s
    // (MaxTimeBeforeForceUpdateTransform = 5.0f, NiagaraComponent.cpp:653, 1771-1780). Bounds read
    // mid-simulation are therefore correct on the grow side and up to five seconds stale on the
    // shrink side, so a set framed from a live read is framed differently per shot for reasons
    // that have nothing to do with the effect. One box, pinned for the call, restored after.
    //
    // WHY A UNION OVER PROBES. A burst is small at t=0 and large at t=0.5; framing every shot to
    // the union is what makes a set comparable frame to frame, and it is the same answer
    // camera.animation_shots reached for moving actors (AnimationShotsHandler.cpp:515-537).
    //
    // Leaves the component AT the last probe instant; the caller re-drives time per shot anyway.
    inline bool MeasureAndPinBounds(UNiagaraComponent& Component, TConstArrayView<double> ProbeTimes,
        float TickDeltaSeconds, FBoundsPlan& OutPlan)
    {
        OutPlan = FBoundsPlan();

        UNiagaraSystem* System = Component.GetAsset();
        if (System)
        {
            OutPlan.BoundsWarning = MakeGpuBoundsWarning(*System, OutPlan.GpuEmittersWithoutFixedBounds);
        }

        FBox Union(ForceInit);
        OutPlan.ProbesAttempted = ProbeTimes.Num();
        for (const double ProbeTime : ProbeTimes)
        {
            FTimeStep Step;
            FString ErrorCode;
            FString ErrorMessage;
            if (!AdvanceToTime(Component, ProbeTime, TickDeltaSeconds, Step, ErrorCode, ErrorMessage))
            {
                continue;
            }
            if (Step.bSimulated)
            {
                ++OutPlan.ProbesSimulated;
            }
            FNiagaraSystemInstanceControllerConstPtr Controller = Component.GetSystemInstanceController();
            if (!Controller.IsValid() || !Controller->IsValid())
            {
                continue;
            }
            const FBox Local = Controller->GetLocalBounds();
            if (Local.IsValid)
            {
                Union += Local;
            }
        }

        // Fall back to whatever the system author put on the asset before giving up. NOT a
        // measurement of this effect - it is the only box a GPU-only system has, and it is also
        // what a system whose instance never came up returns - so the fallback is recorded as a
        // different source and warned about rather than blended into the measured answer.
        bool bFromAuthoredFixedBounds = false;
        if (!Union.IsValid && System)
        {
            const FBox AuthoredFixed = System->GetFixedBounds();
            if (AuthoredFixed.IsValid)
            {
                Union = AuthoredFixed;
                bFromAuthoredFixedBounds = true;
            }
        }

        if (!Union.IsValid)
        {
            // BoundsRadius stays 0, which FResolvedSubject documents as "framing not evaluated".
            // Reported, not an error: a system that has emitted nothing yet is a legitimate thing
            // to photograph, it just cannot solve a camera for itself.
            const FString NoBounds = TEXT(
                "No Niagara bounds could be measured at any sampled instant and the system carries no "
                "authored fixed bounds, so no camera was solved from the subject.");
            OutPlan.BoundsWarning = OutPlan.BoundsWarning.IsEmpty()
                ? NoBounds
                : OutPlan.BoundsWarning + TEXT(" ") + NoBounds;
            return false;
        }

        OutPlan.LocalBounds = Union;
        Component.SetSystemFixedBounds(Union);
        OutPlan.bPinned = true;
        OutPlan.BoundsSource = bFromAuthoredFixedBounds
            ? TEXT("authoredFixedBounds")
            : TEXT("pinnedFixedBounds");
        if (bFromAuthoredFixedBounds)
        {
            // The consequence, not just the fact. A caller reading a radius with no warning has no
            // way to know the number describes the asset's authored box rather than the effect, and
            // a camera solved from it frames whatever the author happened to type - which for the
            // engine's own SimpleExplosion template is a 200 cm cube around the origin.
            const FString AuthoredWarning = FString::Printf(TEXT(
                "These bounds are the system's AUTHORED fixed bounds, not a measurement of the "
                "effect: %d of %d probe instants brought a simulation up, so there were no live "
                "bounds to sample at any of them. The camera is framed to whatever box the asset "
                "carries, which may be unrelated to what the effect fills. Check subject.niagara's "
                "`simulated` and `ageMeasured` - both false means the Niagara instance never "
                "started, and the frames show whatever the preview happened to be displaying; "
                "`simulated` false with `ageMeasured` true means the instance started and the ticks "
                "did not move it, and `simulationStalledReason` names the engine path that swallowed "
                "them."),
                OutPlan.ProbesSimulated, OutPlan.ProbesAttempted);
            OutPlan.BoundsWarning = OutPlan.BoundsWarning.IsEmpty()
                ? AuthoredWarning
                : OutPlan.BoundsWarning + TEXT(" ") + AuthoredWarning;
        }

        const FBoxSphereBounds WorldBounds =
            FBoxSphereBounds(Union).TransformBy(Component.GetComponentTransform());
        OutPlan.WorldOrigin = WorldBounds.Origin;
        OutPlan.WorldRadius = static_cast<double>(WorldBounds.SphereRadius);
        return true;
    }

    // ---- Determinism, reported and never promised -------------------------------------------

    struct FDeterminismReport
    {
        bool bSystemDeterminism = false;
        int32 SystemRandomSeed = 0;
        int32 ComponentRandomSeedOffset = 0;
        int32 EmitterCount = 0;
        // Enabled emitters whose own bDeterminism is off.
        TArray<FString> NonDeterministicEmitters;
        // Enabled GPU emitters. No determinism knob exists anywhere in Niagara for GPU sims, so
        // one of these voids the promise on its own no matter how the three flags are set.
        TArray<FString> GpuEmitters;
    };

    inline FDeterminismReport ReadDeterminism(const UNiagaraSystem& System, const UNiagaraComponent* Component)
    {
        FDeterminismReport Report;
        // NeedsDeterminism()/GetRandomSeed() are the public inline readers for bDeterminism and
        // RandomSeed (NiagaraSystem.h:423-424); the fields themselves are protected.
        Report.bSystemDeterminism = System.NeedsDeterminism();
        Report.SystemRandomSeed = System.GetRandomSeed();
        Report.ComponentRandomSeedOffset = Component ? Component->GetRandomSeedOffset() : 0;
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            if (!Handle.GetIsEnabled())
            {
                continue;
            }
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            if (!EmitterData)
            {
                continue;
            }
            ++Report.EmitterCount;
            if (!EmitterData->bDeterminism)
            {
                Report.NonDeterministicEmitters.Add(Handle.GetName().ToString());
            }
            if (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim)
            {
                Report.GpuEmitters.Add(Handle.GetName().ToString());
            }
        }
        return Report;
    }

    // Names which of the three determinism scopes is off, or which emitters put the question
    // beyond reach. Empty only when every scope that can be checked is on - and even then
    // FResolvedSubject::bTimeReproducible stays false, because data interfaces that read world
    // state are covered by none of them.
    inline FString MakeReproducibilityWarning(const FDeterminismReport& Report)
    {
        TArray<FString> Reasons;
        if (!Report.bSystemDeterminism)
        {
            Reasons.Add(TEXT(
                "system determinism is off, so the instance seed is FMath::Rand() on every reset "
                "(NiagaraSystemInstance.cpp:894)"));
        }
        if (Report.NonDeterministicEmitters.Num() > 0)
        {
            Reasons.Add(FString::Printf(TEXT("emitter determinism is off on %s"),
                *FString::Join(Report.NonDeterministicEmitters, TEXT(", "))));
        }
        if (Report.GpuEmitters.Num() > 0)
        {
            Reasons.Add(FString::Printf(
                TEXT("%s simulate%s on the GPU, which no Niagara determinism setting covers"),
                *FString::Join(Report.GpuEmitters, TEXT(", ")),
                Report.GpuEmitters.Num() == 1 ? TEXT("s") : TEXT("")));
        }
        if (Reasons.Num() == 0)
        {
            return FString();
        }
        return FString::Printf(
            TEXT("These frames are not reproducible: %s. The tick delta and tick count are ")
            TEXT("published so the run can be described exactly, but re-running it may not ")
            TEXT("produce the same particles."),
            *FString::Join(Reasons, TEXT("; ")));
    }

    // ---- The response block ------------------------------------------------------------------

    // The `subject.niagara` detail: what was done to the simulation, and nothing else.
    //
    // There is deliberately NO field of any `reproducible` spelling here, in either direction -
    // including a `timeReproducible` mirror. C1's MakeSubjectInfoObject holds the same line for
    // the same reason (CaptureSubject.cpp:668-675): a boolean would be read as a promise no code
    // here can keep. The warning is the whole statement, and it is carried on
    // FResolvedSubject::ReproducibilityWarning where every kind's warnings live.
    inline TSharedPtr<FJsonObject> MakeNiagaraSubjectDetailObject(
        const FTimeStep& Step, const FBoundsPlan& Bounds, const FDeterminismReport& Determinism)
    {
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        Detail->SetNumberField(TEXT("tickDeltaSeconds"), static_cast<double>(Step.TickDeltaSeconds));
        Detail->SetNumberField(TEXT("tickCount"), Step.TickCount);
        Detail->SetNumberField(TEXT("requestedTimeSeconds"), Step.RequestedSeconds);
        // Present only when it was actually read off the running simulation. Absent means "no
        // instance existed to ask", which is a different statement from "the age was 0".
        if (Step.bAgeMeasured)
        {
            Detail->SetNumberField(TEXT("achievedAgeSeconds"), Step.AchievedAgeSeconds);
        }
        Detail->SetBoolField(TEXT("ageMeasured"), Step.bAgeMeasured);
        // The other end of the same measurement, published so `simulated` below is a verdict the
        // reader can re-derive rather than a boolean they have to trust. Same presence rule, for
        // the same reason: absent means no instance existed to ask.
        if (Step.bStartAgeMeasured)
        {
            Detail->SetNumberField(TEXT("startAgeSeconds"), Step.StartAgeSeconds);
        }
        // UNCONDITIONAL, and it is the field that makes `tickCount` honest: `tickCount: 33` beside
        // `simulated: false` means 33 ticks were ASKED FOR and the simulation did not move, so this
        // instant's frames are not of the moment they claim.
        //
        // MEASURED as the gap between startAgeSeconds and achievedAgeSeconds, not as "a controller
        // existed" - the two disagree on every path listed on FTimeStep::bSimulated, and this
        // response used to carry `simulated: true` and `achievedAgeSeconds: 0` side by side because
        // of it (board ticket B-niagara-preview-age-zero-after-ticks).
        Detail->SetBoolField(TEXT("simulated"), Step.bSimulated);
        // Present only when `simulated` is false, and READ off the instance rather than guessed:
        // which engine path swallowed the ticks, so the next reader does not have to re-derive it
        // from a picture of frame 0.
        if (!Step.StallReason.IsEmpty())
        {
            Detail->SetStringField(TEXT("simulationStalledReason"), Step.StallReason);
        }

        TSharedPtr<FJsonObject> DeterminismObject = MakeShared<FJsonObject>();
        DeterminismObject->SetBoolField(TEXT("system"), Determinism.bSystemDeterminism);
        DeterminismObject->SetNumberField(TEXT("systemRandomSeed"), Determinism.SystemRandomSeed);
        DeterminismObject->SetNumberField(TEXT("componentRandomSeedOffset"),
            Determinism.ComponentRandomSeedOffset);
        DeterminismObject->SetNumberField(TEXT("emitterCount"), Determinism.EmitterCount);
        const auto AddNames = [](const TArray<FString>& Names, const TCHAR* Field,
                                 const TSharedPtr<FJsonObject>& Target)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const FString& Name : Names)
            {
                Values.Add(MakeShared<FJsonValueString>(Name));
            }
            Target->SetArrayField(Field, Values);
        };
        AddNames(Determinism.NonDeterministicEmitters, TEXT("nonDeterministicEmitters"), DeterminismObject);
        AddNames(Determinism.GpuEmitters, TEXT("gpuEmitters"), DeterminismObject);
        Detail->SetObjectField(TEXT("determinism"), DeterminismObject);

        if (!Bounds.BoundsSource.IsEmpty())
        {
            Detail->SetStringField(TEXT("boundsSource"), Bounds.BoundsSource);
        }
        // The evidence behind that source string, so a reader does not have to trust the label. Only
        // when probing was actually attempted: zeros on a plan that never ran would read as five
        // probes that all failed.
        if (Bounds.ProbesAttempted > 0)
        {
            Detail->SetNumberField(TEXT("boundsProbesAttempted"), Bounds.ProbesAttempted);
            Detail->SetNumberField(TEXT("boundsProbesSimulated"), Bounds.ProbesSimulated);
        }
        return Detail;
    }

    // ---- Per-call state, handed between Acquire and Release ----------------------------------

    // Stored in FResolvedSubject::ProviderState. Everything Release must undo, plus the detail
    // block the verb publishes while the resolve is still live.
    //
    // UE builds without RTTI, so recovering this from the type-erased base is gated on
    // FResolvedSubject::Kind - the same discipline C1 uses to make the FAssetEditorToolkit
    // downcast defined. Never static_cast a ProviderState without checking Kind first.
    struct FNiagaraReleaseState : public PinWrightCaptureSubject::FSubjectReleaseState
    {
        TWeakObjectPtr<UNiagaraComponent> Component;
        TWeakObjectPtr<UObject> Asset;
        FComponentState SavedState;
        FTimeStep LastStep;
        FBoundsPlan Bounds;
        FDeterminismReport Determinism;
        bool bCloseAfterCapture = true;
        bool bCloseRequestedExplicitly = false;
        bool bWasAlreadyOpen = false;
    };

    inline FNiagaraReleaseState* GetNiagaraReleaseState(
        const PinWrightCaptureSubject::FResolvedSubject& Subject)
    {
        if (Subject.Kind != PinWrightCaptureSubject::ESubjectKind::Niagara || !Subject.ProviderState.IsValid())
        {
            return nullptr;
        }
        return static_cast<FNiagaraReleaseState*>(Subject.ProviderState.Get());
    }

    // The `subject.niagara` block for a resolved Niagara subject, or an invalid pointer for any
    // other kind. This is how a verb reaches the tick delta, the tick count and the determinism
    // report; the bounds and reproducibility WARNINGS reach it through
    // FResolvedSubject::BoundsWarning / ReproducibilityWarning, which MakeSubjectInfoObject
    // already publishes for every kind.
    inline TSharedPtr<FJsonObject> MakeNiagaraSubjectDetailObject(
        const PinWrightCaptureSubject::FResolvedSubject& Subject)
    {
        const FNiagaraReleaseState* State = GetNiagaraReleaseState(Subject);
        if (!State)
        {
            return nullptr;
        }
        return MakeNiagaraSubjectDetailObject(State->LastStep, State->Bounds, State->Determinism);
    }
}
