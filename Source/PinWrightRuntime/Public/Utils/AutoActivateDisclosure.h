// Copyright (c) 2026 Alexander Penkin. MIT License.

// One text for every verb that can turn bAutoActivate off on a level component, so the three
// of them (actor.set_component_properties, actor.add_component, property.set) cannot describe
// the same consequence differently.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

namespace PinWright
{
    // The disclosure text for a component left NOT auto-activating, or an empty string when
    // there is nothing to disclose (Object is not a component, or the flag is on).
    //
    // A write of bAutoActivate:false is not a session-scoped tweak. The flag is a CDO default
    // of TRUE, so turning it off is serialised into the .umap and the component never starts
    // again on any later load of that level. Three shipped Niagara systems rendered nothing
    // for a full day after a quiesce step wrote exactly this and the map save that followed
    // baked it in, with nothing in the writing verb's response saying more than the property
    // name (B-niagara-validate-green-while-component-inactive, suggested fix 4).
    //
    // The flag is read BACK off the component here rather than taken from the request, so a
    // write the importer declined is never disclosed as one that landed, and restoring the
    // flag - the remedy this text recommends - is silent.
    inline FString MakeAutoActivateDisabledDisclosure(const UObject* Object)
    {
        const UActorComponent* Component = Cast<UActorComponent>(Object);
        if (!IsValid(Component) || Component->bAutoActivate)
        {
            return FString();
        }
        return FString::Printf(
            TEXT("bAutoActivate is now false on '%s': the component will not start on its own, in "
                 "this session or on any later load. bAutoActivate defaults to true, so the "
                 "override is saved into the level and outlives the session - nothing renders from "
                 "this component until it is restored or something activates it explicitly."),
            *Component->GetName());
    }
}
