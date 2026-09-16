// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraComponentActivation.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace PinWrightNiagara
{
    EComponentActivation SurveyPlacedComponents(
        const UNiagaraSystem& System,
        TArray<FPlacedNiagaraComponent>& OutComponents)
    {
        OutComponents.Reset();

        UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return EComponentActivation::Unverified;
        }

        // TActorIterator walks the open world's levels, so this reaches exactly the placed
        // content of the level the author has open - and nothing in a preview or PIE world.
        int32 ActiveCount = 0;
        for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
        {
            AActor* const Actor = *ActorIt;
            if (!IsValid(Actor))
            {
                continue;
            }

            TInlineComponentArray<UNiagaraComponent*> Components;
            Actor->GetComponents(Components);
            for (UNiagaraComponent* const Component : Components)
            {
                if (!IsValid(Component) || Component->GetAsset() != &System)
                {
                    continue;
                }

                FPlacedNiagaraComponent& Entry = OutComponents.AddDefaulted_GetRef();
                Entry.ActorLabel = Actor->GetActorNameOrLabel();
                Entry.ComponentName = Component->GetName();
                Entry.ComponentPath = Component->GetPathName();
                Entry.bIsActive = Component->IsActive();
                Entry.bAutoActivate = Component->bAutoActivate != 0;
                if (Entry.bIsActive)
                {
                    ++ActiveCount;
                }
            }
        }

        if (OutComponents.Num() == 0)
        {
            return EComponentActivation::NoComponents;
        }
        return ActiveCount > 0 ? EComponentActivation::Active : EComponentActivation::NoneActive;
    }

    const TCHAR* ComponentActivationToString(EComponentActivation Activation)
    {
        switch (Activation)
        {
        case EComponentActivation::NoComponents:
            return TEXT("no_components");
        case EComponentActivation::Active:
            return TEXT("active");
        case EComponentActivation::NoneActive:
            return TEXT("none_active");
        default:
            return TEXT("unverified");
        }
    }
}
