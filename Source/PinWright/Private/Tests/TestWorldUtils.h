// Copyright (c) 2026 Alexander Penkin. MIT License.

// Scoped guard for handler tests that spawn actors into the editor world.
// UE automation runs against whatever map is loaded at editor startup, which on a
// host project is a shipped asset, so a spawn-handler test that leaves its actor
// behind dirties — and via editor.save_all / editor shutdown, saves — that asset.
// PinWright.aa_suite_start.OpenBlankTransientWorld now swaps that map out for a
// blank untitled world before the rest of the suite runs, so an unguarded leak no
// longer reaches host content; this guard is still what keeps one test's actors
// out of the next test's world. It records the
// editor world's actor set and the persistent level package's dirty flag on
// construction, then on destruction destroys any actors spawned during the test
// and restores the dirty flag, leaving the open map exactly as the test found it.
#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "LevelUtils.h"
#include "Tests/TestUtils.h"
#include "Utils/MapSwapDirtyWorldGuard.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "UObject/TopLevelAssetPath.h"

// Find a real World asset that exists on disk but is NOT ExcludePackage (typically the
// active map), so a handler's FindLevelByPathLevel misses on a path that nonetheless has
// a .umap. Returns the package name of the first qualifying asset, or empty when none is
// available (a minimal host with no map assets at all).
//
// This is the ONLY way a test may name a host map. Since
// PinWright.aa_suite_start.OpenBlankTransientWorld the editor world is untitled, so a
// test that needs a real on-disk map cannot read one off the ambient world and must not
// hardcode one either — a hardcoded path is a fixture that exists on the host it was
// written against and fails everywhere else.
inline FString FindAlternateOnDiskMap(const FString& ExcludePackage)
{
    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
    TArray<FAssetData> MapAssets;
    AssetRegistry.GetAssetsByClass(
        FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("World")), MapAssets, false);

    for (const FAssetData& MapAsset : MapAssets)
    {
        const FString Candidate = MapAsset.PackageName.ToString();
        if (Candidate != ExcludePackage && FPackageName::DoesPackageExist(Candidate))
        {
            return Candidate;
        }
    }
    return FString();
}

// Spawns a transient AStaticMeshActor carrying the engine unit cube
// (/Engine/BasicShapes/Cube.Cube) at Location with the given editor label, for
// handler tests that need real static-mesh geometry in the world (bounds, merge,
// render). Returns nullptr if there is no editor world, the engine cube mesh is
// unavailable, or the spawn fails — callers AddInfo-skip on null. Pair with
// FScopedEditorWorldActorGuard to clean up the spawned actor on scope exit.
inline AStaticMeshActor* SpawnTransientCubeActor(UWorld* World, const FString& Label, const FVector& Location)
{
    if (!World)
    {
        return nullptr;
    }
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh)
    {
        return nullptr;
    }
    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags = RF_Transient;
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
    if (!Actor)
    {
        return nullptr;
    }
    Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
    Actor->SetActorLabel(Label);
    return Actor;
}

class FScopedEditorWorldActorGuard
{
public:
    FScopedEditorWorldActorGuard()
    {
        World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            PreExisting.Add(*It);
        }
        if (UPackage* LevelPackage = GetPersistentLevelPackage())
        {
            bLevelWasDirty = LevelPackage->IsDirty();
        }
    }

    ~FScopedEditorWorldActorGuard()
    {
        if (!World)
        {
            return;
        }

        TArray<AActor*> SpawnedDuringTest;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (!PreExisting.Contains(*It))
            {
                SpawnedDuringTest.Add(*It);
            }
        }
        // Deselect before destroying. EditorDestroyActor does not remove the actor
        // from the editor selection, so an actor that a test (or a handler like
        // editor.focus_actor, which calls GEditor->SelectActor) left selected keeps a
        // stale typed-element handle in USelection after it is gone. The next code path
        // that walks the selection — e.g. the transform-widget helper run during
        // editor.save_all's content validation — then dereferences the dead element and
        // hits the fatal "Element type ID '0' has not been registered!" assert.
        USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
        USelection* SelectedComponents = GEditor ? GEditor->GetSelectedComponents() : nullptr;
        for (AActor* Actor : SpawnedDuringTest)
        {
            if (IsValid(Actor))
            {
                if (SelectedActors)
                {
                    SelectedActors->Deselect(Actor);
                }
                if (SelectedComponents)
                {
                    for (UActorComponent* Component : Actor->GetComponents())
                    {
                        SelectedComponents->Deselect(Component);
                    }
                }
                World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
            }
        }

        // Destroying actors re-dirties the package, so restore the flag last.
        if (UPackage* LevelPackage = GetPersistentLevelPackage())
        {
            LevelPackage->SetDirtyFlag(bLevelWasDirty);
        }
    }

private:
    UPackage* GetPersistentLevelPackage() const
    {
        return (World && World->PersistentLevel) ? World->PersistentLevel->GetOutermost() : nullptr;
    }

    UWorld* World = nullptr;
    TSet<AActor*> PreExisting;
    bool bLevelWasDirty = false;
};

// Scoped guard for handler tests that load/create a DIFFERENT map and swap the
// active editor world (level.load, level.create, editor.open_level). UE automation
// runs against whatever map is open at editor startup, so a test that leaves a
// swapped-in map active pollutes every subsequent test in the run. This guard
// snapshots the originally-open map's package name as a STRING on construction
// (the original UWorld* may be GC'd by the swap, so it must never be dereferenced
// later — only the captured name is used), then on destruction, if the active
// world changed, reloads it through the production level.load handler so the
// restore shares level.load's resolution and verification path — or, when the
// original world was an untitled one with no package on disk to reload, opens an
// equivalent blank world instead. Sibling of FScopedEditorWorldActorGuard, which
// restores only spawned actors, not a full map swap.
class FScopedEditorWorldMapGuard
{
public:
    FScopedEditorWorldMapGuard()
    {
        OriginalWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        OriginalMapPath = (OriginalWorld && OriginalWorld->GetOutermost())
            ? OriginalWorld->GetOutermost()->GetName()
            : FString();
    }

    ~FScopedEditorWorldMapGuard()
    {
        if (!GEditor || OriginalMapPath.IsEmpty())
        {
            return;
        }
        // Identity compare is the load-bearing "did the active world swap?" guard;
        // OriginalWorld may now be dangling (GC'd by the swap), so it is used only
        // for pointer identity, never dereferenced.
        UWorld* const FinalWorld = GEditor->GetEditorWorldContext().World();
        if (FinalWorld == OriginalWorld)
        {
            return;
        }
        if (FPackageName::DoesPackageExist(OriginalMapPath))
        {
            TSharedPtr<FJsonObject> RestorePayload = MakeShared<FJsonObject>();
            RestorePayload->SetStringField(TEXT("levelPath"), OriginalMapPath);
            InvokeHandler(TEXT("level.load"), RestorePayload);
            return;
        }

        // The suite runs on a blank untitled world from
        // PinWright.aa_suite_start.OpenBlankTransientWorld, whose /Temp package has no file for
        // level.load to reload — so the branch above cannot fire and doing nothing is not
        // "leave it as found". The maps these tests swap in are throwaway probes their own
        // teardown deletes moments later, so a skipped restore leaves the editor world context
        // pointing at a destroyed world and every later test spawns into it. Rebuild an
        // equivalent blank world instead. NewMap is the same non-prompting entry point
        // level.create uses, behind the same pre-swap survivor probe every map-swapping verb
        // runs — NewMap reaches CheckForWorldGCLeaks, which is Fatal by default on a dead world
        // still resident after the cleanse, and a teardown that just destroyed a world by hand
        // is exactly the state that produces one.
        const PinWrightMapSwapGuard::FWorldSurvivorProbeResult Probe =
            PinWrightMapSwapGuard::ProbeResidentWorldSurvivors(
                FString(), /*bTransactionBufferWillBeCleared=*/false);
        // An unavailable probe measured nothing and is not "clear to swap".
        if (!Probe.bProbeUnavailable && !Probe.IsBlocked())
        {
            GEditor->NewMap(/*bIsPartitionedWorld=*/false);
        }
    }

    // Package name of the map that was open when the guard was constructed.
    const FString& GetOriginalMapPath() const { return OriginalMapPath; }
    // Raw pointer captured at construction — for identity comparison ONLY, never
    // dereference (the swap may have GC'd it).
    UWorld* GetOriginalWorld() const { return OriginalWorld; }

private:
    UWorld* OriginalWorld = nullptr;
    FString OriginalMapPath;
};

// Scoped guard for handler tests that toggle a ULevel's editor lock state
// (FLevelUtils::IsLevelLocked / ToggleLevelLock). Locked-level tests must restore
// the prior lock state on every exit path: a check macro that aborts mid-test, or
// an early return, would otherwise leave the persistent level locked and pollute
// every later test in the run. The constructor records the level's current lock
// state and applies the requested desired state; SetLocked() flips it mid-test;
// the destructor restores the originally-recorded state. Sibling of
// FScopedEditorWorldActorGuard / FScopedEditorWorldMapGuard.
class FScopedLevelLock
{
public:
    explicit FScopedLevelLock(ULevel* InLevel, bool bDesiredLocked)
        : Level(InLevel)
    {
        if (!Level)
        {
            return;
        }
        bOriginalLocked = FLevelUtils::IsLevelLocked(Level);
        SetLocked(bDesiredLocked);
    }

    ~FScopedLevelLock()
    {
        SetLocked(bOriginalLocked);
    }

    // Sets the level's lock state, toggling only when it differs from the target.
    void SetLocked(bool bLocked)
    {
        if (Level && FLevelUtils::IsLevelLocked(Level) != bLocked)
        {
            FLevelUtils::ToggleLevelLock(Level);
        }
    }

private:
    ULevel* Level = nullptr;
    bool bOriginalLocked = false;
};

// Owns a UWorld::CreateWorld test fixture that was not registered with the engine. The package's
// dirty state is restored because transient PIE-name fixtures can otherwise appear in dirty-package
// checks even though the world itself is destroyed.
class FScopedTransientWorldGuard
{
public:
    explicit FScopedTransientWorldGuard(UWorld* InWorld)
        : World(InWorld)
        , Package(InWorld ? InWorld->GetOutermost() : nullptr)
        , bPackageWasDirty(Package && Package->IsDirty())
    {
    }

    ~FScopedTransientWorldGuard()
    {
        if (World)
        {
            World->DestroyWorld(/*bInformEngineOfWorld=*/false);
        }
        if (Package)
        {
            Package->SetDirtyFlag(bPackageWasDirty);
        }
    }

private:
    UWorld* World = nullptr;
    UPackage* Package = nullptr;
    bool bPackageWasDirty = false;
};
