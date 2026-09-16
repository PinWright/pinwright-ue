// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"

class UWorld;

namespace PinWrightNiagara
{
    // One live-component-held UNiagaraSystem whose compiled and resolved data-interface counts
    // disagree, i.e. one that asserts inside the VectorVM the next time anything ticks it.
    struct FTickUnsafeNiagaraSystem
    {
        FString SystemPath;
        // Components holding it that carry a live FNiagaraSystemInstanceController.
        int32 LiveComponents = 0;
        TArray<FDataInterfaceCountMismatch> Mismatches;
    };

    // Sweeps the open editor world for live Niagara components and returns, deduplicated by
    // system, the ones a world tick would detonate. OutSystems is reset on entry; the return is
    // OutSystems.Num().
    //
    // This is the pre-flight for verbs that force a level re-evaluation rather than a per-asset
    // question, and the scope is the level for the same reason niagara.audit_level's is: opening a
    // Level Sequence registers the sequence tool and promotes the next frame to a full
    // LEVELTICK_ViewportsOnly pass, which runs FNiagaraWorldManagerTickFunction over EVERY system
    // simulation in the world with no reference to the sequence's bindings. A binding-scoped check
    // would inspect the safest subset, because the bound systems are the ones SetForceSolo pulls
    // out of the batched simulation.
    //
    // Read-only: loads nothing, compiles nothing, dirties nothing. Systems held only by components
    // outside the editor world (asset-editor previews, thumbnails) are not swept - they are not
    // what a level re-evaluation ticks, and refusing a scrub over one would be a false alarm.
    int32 FindTickUnsafeNiagaraSystems(TArray<FTickUnsafeNiagaraSystem>& OutSystems);

    // Single-line, caller-facing summary naming each system, its live component count and the
    // offending scripts with both counts.
    FString DescribeTickUnsafeNiagaraSystems(const TArray<FTickUnsafeNiagaraSystem>& Systems);
}
