// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;

namespace PinWrightNiagara
{
    // One UNiagaraComponent placed in the open editor level that references the surveyed system.
    struct FPlacedNiagaraComponent
    {
        // Editor label of the owning actor, or its object name when it has no label.
        FString ActorLabel;
        // Component name, e.g. "NiagaraComponent0".
        FString ComponentName;
        // Full object path of the component, in the shape actor.set_component_properties and
        // object.call_function take.
        FString ComponentPath;
        // UActorComponent::IsActive(). In an editor (non-game) world this is the measured
        // "is it running" signal: UActorComponent::RegisterComponentWithWorld activates every
        // bAutoActivate component unconditionally outside a game world
        // (ActorComponent.cpp, `if (bAutoActivate) { ... Activate(true); }`), so an inactive
        // component in the open level is one nothing has started.
        bool bIsActive = false;
        // UActorComponent::bAutoActivate. Defaults true on the CDO, so it is only serialised
        // into the .umap when something overrode it - which is what makes it the cheap
        // disk-level discriminator between a level that renders and one that does not.
        bool bAutoActivate = false;
    };

    enum class EComponentActivation : uint8
    {
        // There is no editor world to read, so nothing was surveyed. Not a verdict - callers
        // must not report it as a pass.
        Unverified,
        // The open level's loaded actors carry no component referencing this system. Also not a
        // statement that none exists: World Partition cells that are not loaded, and levels that
        // are not open, are outside what a survey of the editor world can see. Nothing to report
        // either way.
        NoComponents,
        // At least one placed component is active, so something in the open level is running the
        // system.
        Active,
        // Placed components exist and not one of them is active. The system renders nothing in
        // the open level however healthy the asset is.
        NoneActive
    };

    // Surveys the open editor level for placed components referencing System, filling
    // OutComponents (reset on entry) with one entry per component found, and returning the
    // activation verdict over them.
    //
    // Reads GEditor->GetEditorWorldContext().World() only, never the PIE world: the question
    // this answers is whether the level as authored runs the system, and that is a property of
    // the editor world an author can act on. Preview components (the Niagara asset editor's own
    // viewport, thumbnail rendering) live in other worlds and are excluded for the same reason.
    EComponentActivation SurveyPlacedComponents(
        const UNiagaraSystem& System,
        TArray<FPlacedNiagaraComponent>& OutComponents);

    // Stable wire spelling for EComponentActivation: "unverified" / "no_components" /
    // "active" / "none_active". Handlers echo this so a caller can tell a measured verdict
    // from one this survey was unable to make.
    const TCHAR* ComponentActivationToString(EComponentActivation Activation);
}
