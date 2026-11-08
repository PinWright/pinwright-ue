// Copyright (c) 2026 Alexander Penkin. MIT License.

// PostProcessVolumeUtils.h - Shared find-or-spawn helper for the unbound
// APostProcessVolume that PPV-touching handlers use as their write target.
//
// Promoted out of PostProcessHandler.cpp once the caller count reached eight
// (post_process.set_color_grading / set_bloom / set_lumen_gi /
// set_lumen_reflections / set_motion_blur plus lighting.set_exposure /
// lighting.set_ambient_occlusion). Header-only / inline so any handler TU can
// pick it up without growing the link surface.
//
// Contract: returns the first existing unbound PPV in the editor world (the game
// world in a game process), or
// spawns a fresh one with bUnbound=true. On a missing editor actor subsystem
// or a failed spawn, sends an error through Ctx and returns nullptr — callers
// must check for nullptr and early-return (the response has already been
// dispatched). A freshly spawned volume has already been marked spawned-dirty
// (PinWright::MarkLevelActorSpawned); callers still owe
// PinWright::MarkLevelActorModified before their own writes.

#pragma once

#include "CoreMinimal.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "Subsystems/EditorActorSubsystem.h"
#include "Editor.h"
#endif
#include "Dispatch/PinWrightWorld.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"

namespace PinWright
{
    // Game process: the live game world's first unbound PPV, else a runtime-spawned one
    // (nothing is saved in a game, so no dirty ceremony).
    inline APostProcessVolume* FindOrSpawnUnboundPPVInGameWorld(FHandlerContext& Ctx)
    {
        UWorld* World = PinWright::GetTargetWorld();
        if (!World)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No game world is available"));
            return nullptr;
        }
        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
        {
            if (IsValid(*It) && It->bUnbound)
            {
                return *It;
            }
        }
        APostProcessVolume* PPV = World->SpawnActor<APostProcessVolume>(FVector::ZeroVector, FRotator::ZeroRotator);
        if (!PPV)
        {
            Ctx.SendError(ErrorCodes::ERR_EXECUTION_ERROR,
                TEXT("Failed to find/spawn PostProcessVolume"));
            return nullptr;
        }
        PPV->bUnbound = true;
        return PPV;
    }

    inline APostProcessVolume* FindOrSpawnUnboundPPV(FHandlerContext& Ctx)
    {
#if WITH_EDITOR
        if (PinWright::IsGameProcess())
        {
            return FindOrSpawnUnboundPPVInGameWorld(Ctx);
        }
        UEditorActorSubsystem* ActorSS = GEditor
            ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>()
            : nullptr;
        if (!ActorSS)
        {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING,
                TEXT("EditorActorSubsystem not available"));
            return nullptr;
        }

        APostProcessVolume* PPV = nullptr;
        TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->IsA<APostProcessVolume>())
            {
                APostProcessVolume* Candidate = Cast<APostProcessVolume>(Actor);
                if (Candidate->bUnbound)
                {
                    PPV = Candidate;
                    break;
                }
            }
        }

        if (!PPV)
        {
            PPV = Cast<APostProcessVolume>(SpawnActorInActiveWorld<AActor>(
                APostProcessVolume::StaticClass(),
                FVector::ZeroVector, FRotator::ZeroRotator));
            if (PPV)
            {
                // SpawnActorInActiveWorld is passed no label here, so neither the
                // SetActorLabel nor the interactive UEditorEngine::AddActor dirty path
                // fires — under -unattended nothing dirties at all and the whole volume
                // is lost on close. Dirty before writing bUnbound so the Modify()
                // precedes the property write.
                PinWright::MarkLevelActorSpawned(PPV);
                PPV->bUnbound = true;
            }
        }

        if (!PPV)
        {
            Ctx.SendError(ErrorCodes::ERR_EXECUTION_ERROR,
                TEXT("Failed to find/spawn PostProcessVolume"));
        }
        return PPV;
#else
        return FindOrSpawnUnboundPPVInGameWorld(Ctx);
#endif
    }
}
