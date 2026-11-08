// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Dispatch/PinWrightWorld.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

namespace PinWright
{
namespace Detail
{
    // GetEditorWorldContext() check()s when no editor context exists yet (early engine init);
    // this returns the same world once it does, and nullptr before.
    UWorld* FindWorldOfType(const EWorldType::Type Type)
    {
        if (!GEngine)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == Type && Context.World())
            {
                return Context.World();
            }
        }
        return nullptr;
    }
}

    bool IsGameProcess()
    {
        return !GIsEditor;
    }

    UWorld* GetEditorWorldOrNull()
    {
#if WITH_EDITOR
        if (GIsEditor && GEditor)
        {
            return Detail::FindWorldOfType(EWorldType::Editor);
        }
#endif
        return nullptr;
    }

    UWorld* GetTargetWorld()
    {
#if WITH_EDITOR
        if (GIsEditor)
        {
            if (!GEditor)
            {
                return nullptr;
            }
            // Same order as McpActorUtils::ResolveQueryWorld("auto"): PlayWorld, any PIE context, editor world.
            if (GEditor->PlayWorld)
            {
                return GEditor->PlayWorld;
            }
            if (UWorld* Pie = Detail::FindWorldOfType(EWorldType::PIE))
            {
                return Pie;
            }
            return Detail::FindWorldOfType(EWorldType::Editor);
        }
#endif
        if (GEngine && GEngine->GameViewport)
        {
            if (UWorld* World = GEngine->GameViewport->GetWorld())
            {
                return World;
            }
        }
        // No viewport (e.g. -nullrhi game): the first game world context.
        return Detail::FindWorldOfType(EWorldType::Game);
    }

    UGameViewportClient* GetTargetGameViewport()
    {
        UWorld* World = GetTargetWorld();
        if (!World || !GEngine)
        {
            return nullptr;
        }
        if (const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
        {
            return Context->GameViewport;
        }
        return nullptr;
    }
}
