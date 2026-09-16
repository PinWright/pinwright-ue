// Copyright (c) 2026 Alexander Penkin. MIT License.

// Matching header first: UBT's source-file validation rejects a .cpp whose paired .h is not the
// first include ("Expected CaptureSubjectProviders_Niagara.h to be first header included").
#include "Handlers/Render/CaptureSubjectProviders_Niagara.h"

#include "Handlers/ErrorCodes.h"

#include "EditorViewportClient.h"
#include "Slate/SceneViewport.h"
#include "UObject/UObjectGlobals.h"

#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

// The Niagara subject kind: everything that makes a UNiagaraSystem behave like the other five
// subject kinds, and nothing that is already generic. Adding this kind creates one file and edits
// none - providers register themselves at static init, exactly like REGISTER_RPC_HANDLER.
//
// The mechanism lives in CaptureSubjectProviders_Niagara.h so it can be tested without opening an
// asset editor (TestCaptureSubjectNiagara.cpp). This file is the wiring only.
//
// WHAT THIS FILE DOES NOT DO, DELIBERATELY. It owns no widget-tree walk, no allow-list, no
// toolkit-name cast and no close rule: those are C1's, shared by every kind. The entry that made
// Niagara reachable - "SNiagaraSystemViewport", which the old suffix predicate could never match -
// is already in PinWrightCaptureSubject::GetEditorViewportTypeAllowList(), and FName("Niagara") is
// already in GetSupportedAssetEditorToolkitNames(). A fifth copy of that walk would be the exact
// duplication this plan exists to remove.
//
// No .Build.cs change: Niagara, NiagaraCore and NiagaraEditor are already public dependencies
// (PinWright.Build.cs:25), and nothing here needs a NiagaraEditor symbol.
namespace PinWrightCaptureSubjectNiagara
{
    // How many instants the bounds pin samples across the subject's time window. Five is the
    // smallest count that catches a burst's grow-then-shrink shape; the union of them is what gets
    // pinned, so every shot in a set is framed identically.
    constexpr int32 BoundsProbeCount = 5;

    bool Acquire(const PinWrightCaptureSubject::FSubjectRequest& Request,
        PinWrightCaptureSubject::FResolvedSubject& Out,
        PinWrightCaptureSubject::FSubjectTimeSetter& OutTimeSetter,
        FString& OutErrorCode, FString& OutErrorMessage)
    {
        UObject* Asset = LoadObject<UObject>(nullptr, *Request.AssetPath);
        if (!Asset)
        {
            OutErrorCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
            OutErrorMessage = FString::Printf(TEXT("Asset not found: %s"), *Request.AssetPath);
            return false;
        }
        UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
        if (!System)
        {
            OutErrorCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutErrorMessage = FString::Printf(
                TEXT("The niagara subject kind captures a Niagara System ('%s' is a %s). ")
                TEXT("A Niagara Emitter or Module has no system-level preview viewport to photograph; ")
                TEXT("capture the System that owns it."),
                *Request.AssetPath, *Asset->GetClass()->GetName());
            return false;
        }

        // ---- request-only phase: everything knowable before a window exists ----
        //
        // The time axis is a property of the KIND, not of the instance, so it is settled here: a
        // caller whose acquire fails on a missing viewport still learns that a Niagara subject has
        // a time axis, which is what decision 6 turns on. A Niagara system carries no authored
        // duration - it can loop or run indefinitely - so the window is a STATED DEFAULT, not a
        // measurement, published through subject.timeStartSeconds / timeEndSeconds.
        Out.bTimeSupported = true;
        Out.TimeStartSeconds = 0.0;
        Out.TimeEndSeconds = static_cast<double>(DefaultPreviewDurationSeconds);
        Out.CaptureSource = TEXT("niagaraSystemEditorPreview");
        // Unconditional, and not a policy choice. Determinism is off by default at all three
        // Niagara scopes, nothing covers GPU emitters or data interfaces, and the emitter-scope
        // guarantee is void under a variable delta. See the header for the citations.
        Out.bTimeReproducible = false;

        // Installed BEFORE anything is opened, so a failure anywhere below still releases through
        // a state object that knows what to undo. C1 runs Release on every exit path.
        TSharedPtr<FNiagaraReleaseState> State = MakeShared<FNiagaraReleaseState>();
        State->Asset = Asset;
        State->bCloseAfterCapture = Request.bCloseAfterCapture;
        State->bCloseRequestedExplicitly = Request.bCloseAfterCaptureProvided;
        Out.ProviderState = State;

        const FName AcceptedToolkits[] = { NiagaraSystemToolkitName() };
        PinWrightCaptureSubject::FAssetEditorViewportAcquisition Acquisition;
        if (!PinWrightCaptureSubject::AcquireAssetEditorViewport(
                Asset, AcceptedToolkits, Acquisition, OutErrorCode, OutErrorMessage))
        {
            // bWasAlreadyOpen is still worth recording on the failure path: it is what decides
            // whether Release closes a window this call opened.
            State->bWasAlreadyOpen = Acquisition.bWasAlreadyOpen;
            Out.bEditorWasAlreadyOpen = Acquisition.bWasAlreadyOpen;
            return false;
        }
        State->bWasAlreadyOpen = Acquisition.bWasAlreadyOpen;
        Out.bEditorWasAlreadyOpen = Acquisition.bWasAlreadyOpen;

        int32 CandidateCount = 0;
        UNiagaraComponent* Component =
            FindPreviewNiagaraComponent(*Acquisition.ViewportClient, System, CandidateCount);
        if (!Component)
        {
            OutErrorCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
            OutErrorMessage = FString::Printf(
                TEXT("The Niagara preview viewport for %s is open but its preview world holds no ")
                TEXT("UNiagaraComponent, so there is nothing to advance or frame."),
                *Request.AssetPath);
            return false;
        }
        State->Component = Component;
        CaptureComponentState(*Component, State->SavedState);
        State->Determinism = ReadDeterminism(*System, Component);

        TArray<double> ProbeTimes;
        ProbeTimes.Reserve(BoundsProbeCount);
        for (int32 Index = 0; Index < BoundsProbeCount; ++Index)
        {
            ProbeTimes.Add(Out.TimeEndSeconds * (static_cast<double>(Index) / (BoundsProbeCount - 1)));
        }
        MeasureAndPinBounds(*Component, ProbeTimes, DefaultTickDeltaSeconds, State->Bounds);

        Out.ViewportClient = Acquisition.ViewportClient;
        Out.SceneViewport = Acquisition.SceneViewport;
        Out.BoundsOrigin = State->Bounds.WorldOrigin;
        // 0 when nothing could be measured, which FResolvedSubject documents as "framing not
        // evaluated" - BoundsWarning below says why.
        Out.BoundsRadius = State->Bounds.bPinned ? State->Bounds.WorldRadius : 0.0;
        Out.BoundsSource = State->Bounds.BoundsSource;
        Out.BoundsWarning = State->Bounds.BoundsWarning;
        Out.ReproducibilityWarning = MakeReproducibilityWarning(State->Determinism);

        TWeakObjectPtr<UNiagaraComponent> WeakComponent = Component;
        TWeakPtr<FNiagaraReleaseState> WeakState = State;
        OutTimeSetter = [WeakComponent, WeakState](double TimeSeconds, FString& ErrorCode, FString& ErrorMessage) -> bool
        {
            UNiagaraComponent* Live = WeakComponent.Get();
            TSharedPtr<FNiagaraReleaseState> LiveState = WeakState.Pin();
            if (!Live || !LiveState.IsValid())
            {
                ErrorCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
                ErrorMessage = TEXT("The Niagara preview component went away mid-capture, so the "
                                    "requested instant could not be simulated.");
                return false;
            }
            return AdvanceToTime(*Live, TimeSeconds, DefaultTickDeltaSeconds, LiveState->LastStep,
                ErrorCode, ErrorMessage);
        };

        // ---- the subject-coverage seam ----
        //
        // MOVED HERE FROM THE VERB. render.capture_asset_preview used to build this lambda itself,
        // behind a `if (GetNiagaraReleaseState(...))` test - so the verb carried a table of which
        // kinds can be hidden, and every kind that was not Niagara silently had no coverage
        // figure. Which kinds can hide themselves is the PROVIDER'S knowledge; the verb's job is
        // to ask. With the setter on FResolvedSubject the verb has no per-kind branch left and a
        // new provider gains coverage by binding this, not by editing a handler.
        //
        // The restore writes the SAVED flag through RestoreComponentState on release, so an
        // abandoned set cannot leave the effect hidden in the artist's tab.
        Out.VisibilitySetter = [WeakComponent](bool bVisible, FString& ErrorCode,
            FString& ErrorMessage) -> bool
        {
            UNiagaraComponent* Live = WeakComponent.Get();
            if (!Live)
            {
                ErrorCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
                ErrorMessage = TEXT("The Niagara preview component went away mid-capture, so its "
                                    "visibility could not be changed to measure subject coverage.");
                return false;
            }
            SetPreviewComponentVisible(*Live, bVisible);
            return true;
        };

        return true;
    }

    void Release(PinWrightCaptureSubject::FResolvedSubject& Resolved)
    {
        FNiagaraReleaseState* State = GetNiagaraReleaseState(Resolved);
        if (!State)
        {
            return;
        }
        if (UNiagaraComponent* Component = State->Component.Get())
        {
            // Clears the pinned bounds and puts the simulation back on its own clock.
            RestoreComponentState(*Component, State->SavedState);
        }
        if (UObject* Asset = State->Asset.Get())
        {
            // The three-state rule, spelled once in C1 so every kind spells it the same way, and
            // MEASURED rather than assumed - an asset editor can refuse its own close.
            Resolved.bEditorClosed = PinWrightCaptureSubject::CloseAssetEditor(
                Asset, State->bCloseAfterCapture, State->bCloseRequestedExplicitly, State->bWasAlreadyOpen);
        }
    }

    // Static-init registration, the same shape FAutoRegisterHandler uses for RPC handlers: C1's
    // provider table is a function-local static, so this is safe whatever order the translation
    // units initialise in.
    struct FRegistrar
    {
        FRegistrar()
        {
            PinWrightCaptureSubject::FSubjectProvider Provider;
            Provider.Kind = PinWrightCaptureSubject::ESubjectKind::Niagara;
            Provider.Acquire = &Acquire;
            Provider.Release = &Release;
            PinWrightCaptureSubject::RegisterProvider(Provider);
        }
    };

    const FRegistrar GRegistrar;
}
