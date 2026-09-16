// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;
class FNiagaraSystemViewModel;

namespace PinWrightNiagara
{
    // Reuse an editor tab's live SVM when present so user-initiated edits and MCP edits share state; otherwise allocate a session-cached data-processing SVM (12+ sub-viewmodels — too heavy to construct per-RPC).
    PINWRIGHT_API TSharedRef<FNiagaraSystemViewModel> AcquireSystemViewModel(UNiagaraSystem& System);

    // Drops the cached view model for the given system if we own it. No-op when an editor
    // tab owns the live view model.
    void ReleaseSystemViewModel(UNiagaraSystem& System);

    // Unhooks the OnEnginePreExit lambda and flushes the cache. Call from module shutdown.
    void ShutdownSystemViewModelCache();
}
