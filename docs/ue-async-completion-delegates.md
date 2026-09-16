---
type: system
summary: "UE 5.6 editor completion hook reference for async RPC handlers: per-operation delegate/poll table, FTSTicker polling pattern, multicast double-fire guard, dynamic delegate AddLambda restriction, one-shot self-removing bind pattern."
date: 2026-04-26
tags: [async, delegates, completion, ftsticker, jobs, long-running, ue-engine, lighting, navigation, automation-tests]
---

# UE async completion delegates (handler reference)

This page is the canonical reference for wiring UE editor completion into a
long-running RPC handler via `Ctx.StartJob`. Each operation requires a
different hook; several have non-obvious footguns that caused real bugs during
the Jobs migration.

For the caller-facing contract (ticket response shape, JSONL schema, RPCs)
see [wiki-src/system.md](wiki-src/system.md). For the layer overview see [arch](arch.md).

## Completion hook table

| Operation | Header (UE 5.6) | Hook / Predicate | Delegate type | Notes |
|---|---|---|---|---|
| Automation tests | `Developer/AutomationController/Public/IAutomationControllerManager.h` | `OnTestsComplete` + `ReportsHaveErrors()` | `FSimpleMulticastDelegate` | One-shot bind+remove. Use `ReportsHaveErrors()` to derive `bSuccess`. |
| Lighting build | `Editor/UnrealEd/Public/Editor.h` | `FEditorDelegates::OnLightingBuildSucceeded` / `OnLightingBuildFailed` / `OnLightingBuildKept` | `FSimpleMulticastDelegate` (3 separate) | `Failed` and `Kept` can both fire on a cancelled build. Guard with a shared `TSharedRef<bool, ESPMode::ThreadSafe> bFired` — see footgun #1 below. |
| Navigation build | `Runtime/NavigationSystem/Public/NavigationSystem.h` | `OnNavigationGenerationFinishedDelegate` | `DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam` | `AddLambda` does not compile on dynamic multicasts — use `AddDynamic` with a `UObject` helper, or poll `IsNavigationBuildInProgress()` via `FTSTicker`. See footgun #3. |
| Map opened | `Editor/UnrealEd/Public/Editor.h` | `FEditorDelegates::OnMapOpened` | `FOnMapOpened(FString, bool)` | One-shot bind+remove pattern. |
| Screenshot captured | `Engine/Classes/Engine/GameViewportDelegates.h` | `UGameViewportClient::OnScreenshotCaptured` | `FOnScreenshotCaptured` | Fires at end of frame; file flush is async — add a parallel timeout ticker if downstream code needs the file on disk. |
| Shader compile | `Runtime/Engine/Public/ShaderCompiler.h` | `GShaderCompilingManager->IsCompiling()` | poll only — no delegate | `FTSTicker` at 0.5 s; return `false` to remove when `!IsCompiling()`. |
| Process exit | `HAL/PlatformProcess.h` | `FPlatformProcess::GetProcReturnCode` | poll only | Same `FTSTicker` pattern; call `CloseProc` once `GetProcReturnCode` returns `true`. |
| Asset import | `Runtime/Interchange/Engine/Public/InterchangeManager.h` | `UInterchangeManager::OnAssetsImportDoneNative` | dynamic | Per-batch completion. |

## Footguns from the Jobs migration

### 1. Multicast double-fire (lighting build)

`FEditorDelegates::OnLightingBuildFailed` may fire more than once when a
build is cancelled, and `OnLightingBuildKept` can follow it. The UE editor
source comment in `Editor.h` confirms both can fire in the same session.

Without a guard, `FJobRegistry::Complete` would be called twice, corrupting
the ticket's final state.

**Fix:** share a single `TSharedRef<bool, ESPMode::ThreadSafe> bFired` across
all three resolve lambdas. The first one to run flips the flag; the rest
short-circuit. Implementation lives in
`Private/Handlers/Level/LevelBuildBinds.h::BindLightingBuildCompletion`.

```cpp
auto bFired = MakeShared<bool, ESPMode::ThreadSafe>(false);
auto Resolve = [bFired, OnComplete](bool bSuccess) {
    if (*bFired) return;
    *bFired = true;
    OnComplete(bSuccess, ...);
};
FEditorDelegates::OnLightingBuildSucceeded.AddLambda([Resolve]{ Resolve(true); });
FEditorDelegates::OnLightingBuildFailed.AddLambda([Resolve]{ Resolve(false); });
FEditorDelegates::OnLightingBuildKept.AddLambda([Resolve]{ Resolve(false); });
```

### 2. Polling warm-up race (navigation / shader compile)

A naive poll fires a false-positive completion at the first tick when the
predicate hasn't yet transitioned to "in progress". The async UE build kicks
off asynchronously, so the first `FTSTicker` tick at t=0.5 s observes
`IsNavigationBuildInProgress() == false` and incorrectly reports completion.

**Fix:** use a `TSharedRef<bool> bHasSeenBuildStart` that is set to `true`
the first time the predicate returns `inProgress`. Only treat `!inProgress`
as completion after that flag is set. Cap the warm-up at N ticks (e.g. 12 = 6 s);
if the predicate never goes `true`, complete with `"nothing_to_build": true`.

```cpp
auto bSeen = MakeShared<bool>(false);
int32 WarmupTicks = 0;
FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
    [bSeen, &WarmupTicks, OnComplete](float) -> bool
    {
        bool bInProgress = NavSys->IsNavigationBuildInProgress();
        if (bInProgress) { *bSeen = true; }
        if (*bSeen && !bInProgress) { OnComplete(true); return false; }
        if (!*bSeen && ++WarmupTicks >= 12) { OnComplete(false); return false; }
        return true;
    }), 0.5f);
```

### 3. Dynamic multicast cannot AddLambda

`DECLARE_DYNAMIC_MULTICAST_DELEGATE_*` delegates require `AddDynamic` with a
`UFUNCTION`-tagged method on a `UObject`. Passing a lambda directly does not
compile. Options:
- Create a lightweight `UObject` helper that stores the completion callback
  and binds via `AddDynamic`.
- Use poll-based detection instead (the pattern chosen for navigation build).

### 4. One-shot self-removing bind pattern

For delegates that fire repeatedly (e.g. `FEditorDelegates::OnMapOpened`,
`IAutomationControllerManager::OnTestsComplete`), the lambda must remove its
own handle to avoid firing on subsequent unrelated operations:

```cpp
auto HandlePtr = MakeShared<FDelegateHandle, ESPMode::ThreadSafe>();
*HandlePtr = Delegate.AddLambda([HandlePtr, OnComplete](...) {
    Delegate.Remove(*HandlePtr);  // remove before invoking OnComplete
    OnComplete(...);
});
```

Removing before invoking `OnComplete` avoids re-entrancy if `OnComplete`
itself triggers the same event.

## Integration with Ctx.StartJob

All completion mechanisms above go inside `Args.BindNativeDelegate`, the
callback invoked by `Ctx.StartJob` after the immediate HTTP response is sent.

```cpp
REGISTER_RPC_HANDLER("level.build_lighting", ...)
{
    // ... validation ...
    FJobBindArgs Args;
    Args.StartedPayload = ...;
    Args.BindNativeDelegate = [](FJobOnComplete OnComplete) {
        // wire ONE of the patterns above
        BindLightingBuildCompletion(MoveTemp(OnComplete));
    };
    Ctx.StartJob(MoveTemp(Args));
    return true;
}
```

`Ctx.StartJob` guarantees: ticket allocated → immediate response sent →
`BindNativeDelegate` called. This ordering means a synchronously-firing
failure path inside `BindNativeDelegate` (e.g. navigation system reports
nothing to build on the first tick) cannot race the HTTP response.

## See also

- [wiki-src/system.md](wiki-src/system.md) — Caller-facing ticket contract, JSONL schema, settings, method list (absorbed from former `jobs.md`)
- [arch](arch.md) — Jobs layer in the plugin layer diagram
- [lessons](lessons.md) — Actionable rules derived from bugs found during the Jobs migration
- [RPC design](rpc-design.md) — When a verb needs a job handle at all, and the rule that a verb which cannot cancel must say so instead of reporting `cancelled: true`
