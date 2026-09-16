// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Single definition of the "discard a freshly-created, never-saved test asset" teardown that the
// audio/MetaSound fixtures need. Previously copied verbatim into five test files (TestAudioHandlers,
// TestAudioMusicHandler, TestAudioSynthGenerate, TestAudioAnalysisHandler, TestMetaSoundLiteralGaps);
// with `bUseUnity = true` the copy that sat in an ANONYMOUS namespace became visible at global scope
// once Unity merged those TUs, so an unqualified call inside a `using namespace <FileHelpers>;` block
// resolved to two candidates and failed with C2668. Named-namespace header, same fix already applied
// to AssetDumpTestHelpers / WidgetXmlTestHelpers / BpirGraphTestHelpers (see CLAUDE.md > Building).

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/TaskGraphInterfaces.h"
#include "Compat/EngineVersionCompat.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
#include "Selection.h"
#include "SoundWaveCompiler.h"
#endif

namespace PwTestAssetTeardown
{
    inline void DrainGameThreadBeforeTeardown()
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
    }

    inline UPackage* DiscardLoadedAssetNoGc(UObject* Asset)
    {
        if (!Asset)
        {
            return nullptr;
        }

        UPackage* SourcePackage = Asset->GetOutermost();

#if WITH_EDITOR
        if (UWorld* World = Cast<UWorld>(Asset))
        {
            UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            if (World == EditorWorld
                || (EditorWorld && SourcePackage == EditorWorld->GetOutermost()))
            {
                return nullptr;
            }
            World->CleanupWorld();
        }

        // Join the named asset's own cook first. FinishCachePlatformData is the engine's per-wave
        // join (FSoundWaveCompilingManager::PostCompilation calls exactly this) and, unlike the
        // compiling manager, does not depend on the wave having been registered with it. Done
        // before the rename below so its DO_CHECK derived-data-key comparison still sees the wave
        // at the path it was cooked under.
        if (USoundWave* Wave = Cast<USoundWave>(Asset))
        {
            Wave->FinishCachePlatformData();
        }

        // Join every other pending wave before a later suite GC can reclaim unnamed transient
        // waves whose platform-data workers still hold raw references into the UObject.
        FSoundWaveCompilingManager::Get().FinishAllCompilation();
#endif
        // Production create_* handlers (and the fixtures that mimic them so audit_folder can
        // enumerate the asset) register via FAssetRegistryModule::AssetCreated; mirror that with
        // AssetDeleted so the registry does not keep a stale path entry for the object about to
        // be reclaimed.
        FAssetRegistryModule::AssetDeleted(Asset);

#if WITH_EDITOR
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
        {
            UClass* GeneratedClass = Blueprint->GeneratedClass.Get();
            UClass* SkeletonGeneratedClass = Blueprint->SkeletonGeneratedClass.Get();
            UObject* GeneratedClassDefaultObject = GeneratedClass
                ? GeneratedClass->GetDefaultObject(false)
                : nullptr;
            UObject* SkeletonClassDefaultObject = SkeletonGeneratedClass
                ? SkeletonGeneratedClass->GetDefaultObject(false)
                : nullptr;

            // Engine asset deletion removes generated classes before deleting a Blueprint.
            // Without this, UBlueprint::Rename tries to move the classes and their CDOs into the
            // transient package. A compile retained with SkipGarbageCollection can already have a
            // REINST_ class whose CDO owns that target name, and UObject::Rename fatally rejects
            // moving the current SKEL_ CDO on top of it.
            Blueprint->RemoveChildRedirectors();
            Blueprint->RemoveGeneratedClasses();

            const auto MarkGeneratedObjectForDiscard = [](UObject* Object)
                {
                    if (Object)
                    {
                        Object->ClearFlags(RF_Public | RF_Standalone);
                        Object->SetFlags(RF_Transient);
                        Object->RemoveFromRoot();
                        Object->MarkAsGarbage();
                    }
                };

            MarkGeneratedObjectForDiscard(GeneratedClassDefaultObject);
            if (SkeletonClassDefaultObject != GeneratedClassDefaultObject)
            {
                MarkGeneratedObjectForDiscard(SkeletonClassDefaultObject);
            }
            MarkGeneratedObjectForDiscard(GeneratedClass);
            if (SkeletonGeneratedClass != GeneratedClass)
            {
                MarkGeneratedObjectForDiscard(SkeletonGeneratedClass);
            }
        }
#endif

#if WITH_EDITOR
        // Detach the asset from the typed-element world while it is still alive. This mirrors the
        // two steps ObjectTools::DeleteSingleObject takes before deleting (ObjectTools.cpp:3466-3474)
        // and that this discard path replaced.
        //
        // USelection is element-backed: Select() acquires an editor object element handle
        // (Selection.cpp:40, :248-252) and holds it in the element selection set, so a selected
        // asset's FTypedElementInternalData carries an external reference (RefCount > 1). The
        // element data's FObjectElementData::Object is a RAW UObject* (ObjectElementData.h:16) that
        // GC never nulls. If the asset is left selected here, a later garbage collection - any GC,
        // not necessarily one this teardown runs - frees the object and destroys its element via
        // FCoreUObjectDelegates::PreGarbageCollectConditionalBeginDestroy ->
        // DestroyUnreachableEditorObjectElements (EngineElementsLibrary.cpp:168, :306-334), which
        // only *defers* the removal. The deferred removal is drained from
        // UTypedElementRegistry::OnEndFrame (TypedElementRegistry.cpp:337-348), and because
        // RefCount is still > 1 the drain calls LogExternalReferencesOnDestruction
        // (TypedElementData.h:279-300) -> GetDebugId -> Object->GetFullName() on freed memory:
        // EXCEPTION_ACCESS_VIOLATION inside FEngineLoop::Tick, in whatever unrelated test happens
        // to own that frame. The post-GC drain cannot save it either - OnPostGarbageCollect is
        // suppressed for the whole frame by OnBeginFrame's IncrementDisableElementDestructionOnGCCount
        // (TypedElementRegistry.cpp:331-335, :349-356), and GetPostGarbageCollect broadcasts after
        // IncrementalPurgeGarbage anyway (GarbageCollection.cpp:5920, :5943).
        //
        // Deselecting releases the selection's handle, so the element that GC later destroys is
        // owner-referenced only and the drain never reaches GetDebugId. Deselect is deliberately
        // the whole fix: the element owner itself is still reclaimed by the engine's own GC hook,
        // exactly as it was before this discard path replaced the force-delete.
        if (GEditor)
        {
            if (USelection* SelectedObjects = GEditor->GetSelectedObjects())
            {
                SelectedObjects->Deselect(Asset);
            }
        }
#endif

        Asset->ClearFlags(RF_Public | RF_Standalone);
        Asset->SetFlags(RF_Transient);
        Asset->RemoveFromRoot();
        const FString DiscardName = MakeUniqueObjectName(
            GetTransientPackage(), Asset->GetClass(), Asset->GetFName()).ToString();
        const bool bAssetRenamed = Asset->Rename(*DiscardName, GetTransientPackage(),
            REN_DontCreateRedirectors | REN_NonTransactional | REN_SkipGeneratedClasses
                | MCP_REN_NO_RESET_LOADERS);
        if (!bAssetRenamed)
        {
            UE_LOG(LogTemp, Log, TEXT("DiscardLoadedAssetNoGc could not move asset '%s' to the transient package."),
                *Asset->GetPathName());
            return nullptr;
        }

        bool bSourcePackageHasAssets = false;
        if (SourcePackage && SourcePackage != GetTransientPackage())
        {
            ForEachObjectWithPackage(SourcePackage, [&bSourcePackageHasAssets](UObject* Object)
            {
                if (Object->IsAsset())
                {
                    bSourcePackageHasAssets = true;
                    return false;
                }
                return true;
            }, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS);
        }

        if (SourcePackage && SourcePackage != GetTransientPackage() && !bSourcePackageHasAssets)
        {
            SourcePackage->SetDirtyFlag(false);
            SourcePackage->ClearFlags(RF_Public | RF_Standalone);
            SourcePackage->SetFlags(RF_Transient);
            SourcePackage->RemoveFromRoot();
            const FString DiscardPackageName = MakeUniqueObjectName(
                GetTransientPackage(), UPackage::StaticClass(), SourcePackage->GetFName()).ToString();
            const ERenameFlags PackageRenameFlags =
                REN_DontCreateRedirectors | REN_NonTransactional | MCP_REN_NO_RESET_LOADERS;
            // REN_Test leaves SourcePackage at its original name while checking the same move
            // preconditions. The immediately-following non-test rename is guaranteed to succeed,
            // so this notification removes the original registry key without pre-deleting it on a
            // failed preflight.
            const bool bPackageRenameReady = SourcePackage->Rename(
                *DiscardPackageName, GetTransientPackage(), PackageRenameFlags | REN_Test);
            if (!bPackageRenameReady)
            {
                UE_LOG(LogTemp, Log, TEXT("DiscardLoadedAssetNoGc could not move package '%s' to the transient package."),
                    *SourcePackage->GetPathName());
                return nullptr;
            }
            FAssetRegistryModule::PackageDeleted(SourcePackage);
            const bool bPackageRenamed = SourcePackage->Rename(
                *DiscardPackageName, GetTransientPackage(), PackageRenameFlags);
            if (!bPackageRenamed)
            {
                UE_LOG(LogTemp, Log, TEXT("DiscardLoadedAssetNoGc could not move package '%s' to the transient package."),
                    *SourcePackage->GetPathName());
                return nullptr;
            }
        }

        return SourcePackage;
    }

    /**
     * Discards an asset created in-memory by a test or by a handler under test (e.g. via
     * create_metasound / create_sound_cue with save=false).
     *
     * Deliberately NOT UEditorAssetLibrary::DeleteAsset -> ObjectTools::ForceDeleteObjects:
     * force-delete's GatherObjectReferencersForDeletion serializes the freshly-created,
     * never-reloaded asset to collect referencers, and for a UMetaSoundSource (whose frontend
     * document graph holds transient/uninitialized references) that reference-gathering archive
     * crashes the editor under -unattended -RenderOffScreen - the same never-reloaded-asset hazard
     * CleanupTestAsset documents for 5.4. Instead detach the asset from the standalone/public roots,
     * rename it into the transient package and let GC reclaim it, the way the engine itself discards
     * a never-saved asset.
     *
     * Always drains the game-thread task queue FIRST. Asset creation paths queue work onto the
     * game thread that holds RAW pointers to the object being created and does not run inline:
     * Audio::FSoundWavePCMWriter::SerializeSoundWaveToAsset posts FAssetRegistryModule::AssetCreated
     * + MarkPackageDirty through AsyncTask(ENamedThreads::GameThread, ...) capturing a raw
     * USoundWave* (SampleBufferIO.cpp:581-584). Collecting garbage before that lambda drains leaves
     * it dereferencing freed memory - an EXCEPTION_ACCESS_VIOLATION inside
     * UAssetRegistryImpl::AssetCreated on a later frame, which kills the whole suite rather than
     * the one test. Draining is unconditional here because it is never wrong: it only runs work
     * already queued, and every caller is an automation test on the game thread about to GC.
     * ENamedThreads::GameThread (MainQueue) is the queue AsyncTask posts to - see PumpUntilCaptured
     * in Tests/TestUtils.h.
     *
     * Also joins outstanding USoundWave compilation, which the game-thread drain above does NOT
     * cover because that work runs on the asset THREAD POOL. FAudioCookInputs keeps the only refs
     * it has on a wave as raw C++ references into the live UObject - FCriticalSection&
     * BulkDataCriticalSection (= Wave->RawDataCriticalSection) and FEditorAudioBulkData& BulkData
     * (= Wave->RawData), AudioDerivedData.cpp:1659-1664 - for the whole life of
     * FStreamedAudioCacheDerivedDataWorker, and nothing keeps the wave alive for that worker:
     * BeginDestroy flushes only AsyncLoadingDataFormats, FinishDestroy clears only
     * CookedPlatformData, and the RunningPlatformData task holding those references is joined only
     * by ~FSoundWaveData - which runs AFTER ~FCriticalSection, because WorkingSoundWaveData is
     * declared before RawDataCriticalSection (SoundWave.h:537 vs :1125) and members die in reverse
     * declaration order. Collecting a still-cooking wave therefore hands the pool worker a
     * DeleteCriticalSection'd CRITICAL_SECTION: FScopeLock at AudioDerivedData.cpp:1817 takes
     * ntdll's contention path and writes RTL_CRITICAL_SECTION_DEBUG::ContentionCount through the
     * NULLed DebugInfo pointer - EXCEPTION_ACCESS_VIOLATION writing 0x24 on a background thread,
     * which kills the whole suite. Every export starts such a cook: SetSoundAssetCompressionType
     * -> UpdateAsset -> InvalidateCompressedData -> CachePlatformData(bAsyncCache=true).
     */
    inline void DiscardCreatedAssetByObjectPath(const FString& ObjectPath)
    {
        DrainGameThreadBeforeTeardown();

        UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
        if (!Asset)
        {
            return;
        }
        DiscardLoadedAssetNoGc(Asset);
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }
}
