// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraSystemViewModelCache.h"

#include "Delegates/IDelegateInstance.h"
#include "Misc/CoreDelegates.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraSystem.h"
#include "ViewModels/NiagaraSystemViewModel.h"
// Note: TNiagaraViewModelManager::GetExistingViewModelForObject can't be called from outside
// NiagaraEditor — its inline body references the static ObjectsToViewModels member, whose
// per-template-instantiation definition lives in NiagaraEditor's private TUs and isn't exported.
// We rely solely on this module's local cache instead of trying to share with editor SVMs.

namespace PinWrightNiagara
{
    namespace
    {
        TMap<TWeakObjectPtr<UNiagaraSystem>, TSharedRef<FNiagaraSystemViewModel>>& GetCacheRef()
        {
            static TMap<TWeakObjectPtr<UNiagaraSystem>, TSharedRef<FNiagaraSystemViewModel>> Cache;
            return Cache;
        }

        FDelegateHandle& GetShutdownHandle()
        {
            static FDelegateHandle Handle;
            return Handle;
        }

        // First touch wires a PreExit handler so we drop our shared refs before the engine
        // tears the Niagara module down. Without this the SVM destructor can fire after
        // its dependencies have been unloaded. The handle is captured so module shutdown
        // can unhook cleanly.
        void EnsureShutdownHookInstalled()
        {
            FDelegateHandle& Handle = GetShutdownHandle();
            if (Handle.IsValid())
            {
                return;
            }
            Handle = FCoreDelegates::OnEnginePreExit.AddLambda([]()
            {
                GetCacheRef().Empty();
            });
        }

        // TWeakObjectPtr keys go stale on GC, but the matching TSharedRef value pins the SVM
        // (12+ sub-viewmodels) until process exit. Sweep stale entries on every Acquire to
        // bound session-long memory growth without needing a separate GC subscription.
        void PurgeStaleEntries()
        {
            TMap<TWeakObjectPtr<UNiagaraSystem>, TSharedRef<FNiagaraSystemViewModel>>& Cache = GetCacheRef();
            for (auto It = Cache.CreateIterator(); It; ++It)
            {
                if (!It.Key().IsValid())
                {
                    It.RemoveCurrent();
                }
            }
        }
    }

    TSharedRef<FNiagaraSystemViewModel> AcquireSystemViewModel(UNiagaraSystem& System)
    {
        EnsureShutdownHookInstalled();
        PurgeStaleEntries();

        TMap<TWeakObjectPtr<UNiagaraSystem>, TSharedRef<FNiagaraSystemViewModel>>& Cache = GetCacheRef();
        if (TSharedRef<FNiagaraSystemViewModel>* Cached = Cache.Find(&System))
        {
            return *Cached;
        }

        // FNiagaraSystemViewModel::Initialize unconditionally calls RefreshAll
        // (NiagaraSystemViewModel.cpp:198), which calls CompileSystem(false)
        // (line 1941) whenever HasOutstandingCompilationRequests() == false.
        // CompileSystem routes through UNiagaraScript::ComputeVMCompilationId,
        // which derefs VersionData[0] at NiagaraScript.cpp:1005. A bare
        // NewObject<UNiagaraSystem>(GetTransientPackage()) reaches PostInitProperties
        // (NiagaraSystem.cpp:553) which constructs SystemSpawnScript/SystemUpdateScript
        // but never sets up VersionData on them — that wiring happens in PostLoad.
        // Result: VersionData[0] is index 0 into a size-0 array → crash.
        // bCanAutoCompile/bIsForDataProcessingOnly do NOT gate the initial compile
        // inside RefreshAll, so the only safe guard is upstream: refuse to acquire
        // an SVM for a system whose system-spawn/update scripts have no source.
        // Callers must duplicate from a loaded fixture asset (see
        // NiagaraEditTestUtils::NewTransientSystem) rather than pass a bare NewObject.
        UNiagaraScript* SpawnScript = System.GetSystemSpawnScript();
        UNiagaraScript* UpdateScript = System.GetSystemUpdateScript();
        const bool bHasSpawnSource = SpawnScript && SpawnScript->GetLatestSource() != nullptr;
        const bool bHasUpdateSource = UpdateScript && UpdateScript->GetLatestSource() != nullptr;
        checkf(bHasSpawnSource && bHasUpdateSource,
            TEXT("AcquireSystemViewModel called on a bare/uninitialized UNiagaraSystem ('%s'). ")
            TEXT("System-spawn/update scripts have no source — FNiagaraSystemViewModel::Initialize ")
            TEXT("would crash inside UNiagaraScript::ComputeVMCompilationId (VersionData[0] OOB). ")
            TEXT("Construct via NiagaraEditTestUtils::NewTransientSystem or load a real asset."),
            *System.GetPathName());

        TSharedRef<FNiagaraSystemViewModel> SVM = MakeShared<FNiagaraSystemViewModel>();
        FNiagaraSystemViewModelOptions Options;
        Options.bCanModifyEmittersFromTimeline = false;
        Options.bCanAutoCompile = false;
        Options.bCanSimulate = false;
        Options.bIsForDataProcessingOnly = true;
        Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
        // Mirror NiagaraSystemToolkit / NiagaraEditorUtilities: seed the message-log key from
        // the system's AssetGuid. Without this, FNiagaraSystemViewModel::SystemMessageLogGuidKey
        // stays unset; downstream stack-object construction passes FGuid() to
        // FNiagaraMessageManager::SubscribeToAssetMessagesByObject and trips its
        // `MessageAssetKey != FGuid()` checkf.
        Options.MessageLogGuid = System.GetAssetGuid();
        SVM->Initialize(System, Options);

        Cache.Add(&System, SVM);
        return SVM;
    }

    void ReleaseSystemViewModel(UNiagaraSystem& System)
    {
        TMap<TWeakObjectPtr<UNiagaraSystem>, TSharedRef<FNiagaraSystemViewModel>>& Cache = GetCacheRef();
        Cache.Remove(&System);
    }

    void ShutdownSystemViewModelCache()
    {
        FDelegateHandle& Handle = GetShutdownHandle();
        if (Handle.IsValid())
        {
            FCoreDelegates::OnEnginePreExit.Remove(Handle);
            Handle.Reset();
        }
        GetCacheRef().Empty();
    }
}
