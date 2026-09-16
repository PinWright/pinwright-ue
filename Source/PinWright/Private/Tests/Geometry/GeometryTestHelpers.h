// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared geometry-test cleanup helper. Extracted from anonymous namespaces
// previously duplicated across TestGeometryCreateNameParamAlias.cpp and
// TestGeometryConvertStaticMeshEcho.cpp. Required because the plugin's tests
// share a single module with Unity builds: anonymous-namespace helpers with the
// same name across .cpp files produce ODR / redefinition errors when Unity
// merges them into one translation unit (surfaced first on the UE 5.3 host,
// whose Unity bucketing landed both files in the same TU).
//
// Conventions match Tests/Infra/DispatcherTestHelpers.h: named namespace +
// inline functions, no module API macro.

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace GeometryTestHelpers
{
    // Return the first actor in the editor world whose label matches Label, or
    // nullptr if none (or there is no editor world). Companion to
    // DestroyActorsWithLabel for tests that fetch a just-created, uniquely
    // labelled actor instead of re-inlining the TActorIterator + GetActorLabel
    // search loop.
    inline AActor* FindActorByLabel(const FString& Label)
    {
        if (!GEditor)
        {
            return nullptr;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!IsValid(World))
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label))
            {
                return *It;
            }
        }
        return nullptr;
    }

    // Remove any actor whose label matches Label from the editor world so the
    // test leaves no residue on the (mutated) fuzzing host.
    inline void DestroyActorsWithLabel(const FString& Label)
    {
        if (!GEditor)
        {
            return;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!IsValid(World))
        {
            return;
        }
        TArray<AActor*> ToDestroy;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label))
            {
                ToDestroy.Add(*It);
            }
        }
        for (AActor* Actor : ToDestroy)
        {
            World->DestroyActor(Actor);
        }
    }
}
