// Copyright (c) 2026 Alexander Penkin. MIT License.

// The one safe-point gate for RPC work that may not run inside UWorld::Tick.
//
// Moved here (and generalized) from the deleted Handlers/Level/MapSwapSafePoint.h,
// because the hazard is not level-specific: four sibling level/lighting verbs and
// the whole capture family reach the same mid-frame position. Hand-copying the gate
// into level.load alone is exactly how those siblings were missed, so this is the
// single place the gate lives. Namespace PinWrightMapSwap became PinWrightSafePoint
// and log category LogPinWrightMapSwap became LogPinWrightSafePoint in the same
// move; there is deliberately no compatibility alias, so a stale call site is a
// compile error rather than a second mechanism.
//
// WHY THIS EXISTS - the mechanism behind `Assertion failed: !LevelList.Contains(TickTaskLevel)`
// (board: B-open-asset-world-map-load-crash). Line numbers are UE 5.8
// (C:/UE_5.8/Engine/Source); the same shape holds on 5.3-5.7, only the line
// numbers move (the shipped crash logs show TickTaskManager.cpp:1987 on 5.7 and
// :1992 on 5.8).
//
// 1. FTickTaskManager keeps a `LevelList` of the FTickTaskLevel objects it is
//    ticking THIS FRAME. It is populated by FillLevelList from StartFrame
//    (TickTaskManager.cpp:2031 -> :2233 `check(!LevelList.Num())`, :2240
//    `LevelList.Add(Level->TickTaskLevel)`) and emptied by EndFrame
//    (:2191 `LevelList.Reset()`). Outside that window the list is EMPTY.
//
// 2. ULevel::~ULevel frees its tick level (Level.cpp:470-473 ->
//    FTickTaskManagerInterface::Get().FreeTickTaskLevel(TickTaskLevel)), and
//    FreeTickTaskLevel asserts the level is not still listed
//    (TickTaskManager.cpp:1990-1993):
//
//        virtual void FreeTickTaskLevel(FTickTaskLevel* TickTaskLevel) override
//        {
//            check(!LevelList.Contains(TickTaskLevel));
//            delete TickTaskLevel;
//        }
//
//    So destroying a ULevel is only legal while LevelList is empty - i.e. NOT
//    between StartFrame and EndFrame. ULevel.cpp:472 is the ONLY caller of
//    FreeTickTaskLevel in the engine (verified by grep over
//    Engine/Source/Runtime/Engine), so "destroy a ULevel" and "trip that check"
//    are the same event.
//
// 3. FEditorFileUtils::LoadMap tears the outgoing world down synchronously:
//    Map_Load (EditorServer.cpp:2343) -> EditorDestroyWorld (:2480) ->
//    Cleanse (EditorEngine.cpp:2763) -> CollectGarbage (:2859) -> ~ULevel ->
//    FreeTickTaskLevel. If that happens mid-frame, the outgoing world's level is
//    still in LevelList and the check fires. The engine knows this hazard and
//    works around it for its own subsystem (LevelTick.cpp:1891):
//
//        // Tick LevelInstanceSubsystem outside of FTickTaskManagerInterface::StartFrame/EndFrame
//        // because it can cause levels to be deleted and invalidate its LevelList
//
//    LoadMap is not the only route. The two other verified synchronous paths from
//    an RPC body to that same CollectGarbage are:
//      - UEditorEngine::NewMap (EditorServer.cpp:2187) -> EditorDestroyWorld
//        (:2206) -> Cleanse (:2080) -> CollectGarbage (EditorEngine.cpp:2859).
//        Reached by `level.create` and `lighting.create_lighting_enabled_level`.
//      - UEditorLevelUtils::RemoveLevelFromWorld (EditorLevelUtils.cpp:973) ->
//        RemoveLevelsFromWorld (:844) -> GEditor->Cleanse (:933) ->
//        CollectGarbage. Reached by `level.remove_from_world`.
//
// 4. PinWright reaches that mid-frame position by accident, not by design.
//    FRpcDispatcher::ProcessRequest marshals off-thread requests with
//    `AsyncTask(ENamedThreads::GameThread, ...)` (RpcDispatcher.cpp:380-385).
//    The game thread drains its task-graph queue from INSIDE the world tick
//    while waiting on tick groups - ProcessUntilTasksComplete
//    (TickTaskManager.cpp:1040), WaitUntilTasksComplete (:1045) and
//    ProcessThreadUntilIdle (:1064, the TG_DuringPhysics non-blocking path).
//    So whether an RPC runs at a safe point or in the middle of UWorld::Tick
//    depends purely on WHEN the HTTP thread happened to enqueue it. That is why
//    the crash reproduces for a single client with no concurrency, and why it
//    looks intermittent. The shipped callstack shows exactly this:
//    UWorld::Tick -> RunTickGroup -> ReleaseTickGroup -> task graph ->
//    FRpcDispatcher::ProcessRequest -> LevelHandler.cpp -> LoadMap -> ... ->
//    FreeTickTaskLevel.
//
// THE SAFE POINT. FTSTicker::GetCoreTicker() is pumped from FEngineLoop::Tick at
// LaunchEngineLoop.cpp:6103, well AFTER `GEngine->Tick(...)` returns at :5859.
// GEngine->Tick is what runs UWorld::Tick, so by the time a core-ticker callback
// runs, EndFrame has already emptied LevelList. No other engine call site pumps
// the core ticker from inside a world tick (the nested pumps all live in
// commandlets and modal progress loops). A core-ticker callback is therefore a
// provably safe place to swap maps, tear levels down, or drive a viewport.
//
// UPinWrightSubsystem::Tick is registered on that same core ticker
// (PinWrightSubsystem.cpp:209, 0.1s period) and drains the dispatcher's deferred
// queue from there (PinWrightSubsystem.cpp:303). That makes FRpcDispatcher's
// PendingQueue a safe point too, which is what the method-level gate below uses:
// a listed method that arrives mid-tick is re-queued and runs from the ticker
// instead, at most one 0.1s tick later, with its FHandlerContext and response
// path completely unchanged.
//
// THE GATE. UWorld::bInTick (World.h:1262, public, "Whether we are in the middle
// of ticking actors/components or not") is set true at LevelTick.cpp:1564 -
// BEFORE StartFrame at :1742 - and false at :1965, AFTER EndFrame at :1886. Its
// window therefore strictly contains the LevelList window, so `bInTick` is a
// conservative gate: it can only over-defer, never under-defer. It also covers
// the wider "do not re-enter Slate/rendering from inside the world tick" hazard
// the capture verbs have, which is not a LevelList problem at all.
//
// The one hole is the two-line `bInTick = false; EnsureCollisionTreeIsBuilt();
// bInTick = true;` at LevelTick.cpp:1752-1754, which sits INSIDE the LevelList
// window with bInTick reading false. It is provably unreachable for this purpose:
// UWorld::EnsureCollisionTreeIsBuilt (World.cpp:3141-3148) opens with
//
//     if (bInTick || bIsBuilt) { return; }
//     if (GIsEditor && !IsPlayInEditor()) { return; }
//
// so in an editor world it returns on the second line having pumped nothing. A PIE
// world could reach the physics-scene branch, but FEditorFileUtils::LoadMap refuses
// to run at all while a PIE world exists (ShouldAbortBecauseOfPIEWorld,
// FileHelpers.cpp:3280), so no map swap can land there either.
//
// THE OTHER HALF OF THE GATE. `bInTick` answers "is a world ticking", which is not
// the same question as "is this stack inside the engine frame". A third-party editor
// tickable that blocks on a task pumps the game thread's named-thread queue from
// FTickableEditorObject::TickObjects (EditorEngine.cpp:1933) - which runs BEFORE the
// editor world tick at :1967 - so a queued PinWright request can run inside the frame
// with `bInTick` false for every world. `bInTick` cannot see that window at all.
// IsInsideNamedThreadPump() below is what closes it, and the two predicates together
// are IsSafeNow(). Board: B-safepoint-tick-gate-inert-on-simpletickobjects-path.
//
// NOT A HAZARD, so deliberately NOT gated: `GEditor->ForceGarbageCollection(true)`
// only raises a flag; the collect it schedules is consumed by
// GEngine->ConditionalCollectGarbage() at LevelTick.cpp:1970, which runs AFTER
// `bInTick = false` (:1965) and after EndFrame (:1886). A forced GC therefore
// never lands inside the LevelList window on its own.
//
// WHY THE CORE TICKER AND NOT A TIMER. GEditor->GetTimerManager()->SetTimerForNextTick
// is equally safe (pumped at EditorEngine.cpp:1783, before the editor world tick at
// :1967) and the plugin uses it elsewhere. The core ticker is chosen because it is
// this plugin's dominant deferral idiom (~24 call sites across the handler tree), so
// the existing test helpers already know how to pump it. Either would fix the crash.
// What must NOT be used is another AsyncTask(ENamedThreads::GameThread, ...): that is
// the very queue drained mid-frame, so it would re-arm the same bug - which is why the
// removed Ctx.StartJob + OnMapOpened wrapper never fixed it. Note that Ctx.StartJob is
// NOT a deferral primitive either: FHandlerContext::StartJob invokes
// Args.BindNativeDelegate synchronously (HandlerContext.cpp:627), so a job whose bind
// delegate does its work inline still runs on the caller's stack.

#pragma once

#include "CoreMinimal.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dispatch/WorldPrecondition.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Handlers/HandlerContext.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

// Defined in Dispatch/SafePoint.cpp. Every deferral logs one line here so a hop is
// greppable in the editor log without a debugger.
PINWRIGHT_API DECLARE_LOG_CATEGORY_EXTERN(LogPinWrightSafePoint, Log, All);

namespace PinWrightSafePoint
{
namespace Detail
{
    // Function-local static in an inline function: exactly one instance across
    // every translation unit, which keeps this header-only under Unity builds
    // (same shape as PinWrightAutomationMode::Detail).
    inline bool& ForcedUnsafeForTests()
    {
        static bool bGForcedUnsafe = false;
        return bGForcedUnsafe;
    }

    // One extra method name the gate treats as tick-unsafe. Lets the dispatcher
    // gate be driven end-to-end against a `_test.*` fixture handler instead of a
    // real capture verb, which a test must never actually execute.
    inline FString& ExtraTickUnsafeMethodForTests()
    {
        static FString GExtraMethod;
        return GExtraMethod;
    }
}

// True while ANY live world context is inside UWorld::Tick. Every world context
// is checked, not just the editor world: a PIE world ticking in the same process
// populates the same process-wide FTickTaskManager LevelList.
inline bool IsAnyWorldTicking()
{
    if (!GEngine)
    {
        return false;
    }
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        const UWorld* World = Context.World();
        if (World && World->bInTick)
        {
            return true;
        }
    }
    return false;
}

// True while the game thread is executing task-graph work out of one of its own
// named-thread queues. This is the half of the unsafe window `UWorld::bInTick`
// cannot see (board: B-safepoint-tick-gate-inert-on-simpletickobjects-path).
//
// WHY IT IS NEEDED. FRpcDispatcher::ProcessRequest marshals every off-thread request
// with AsyncTask(ENamedThreads::GameThread, ...), so a request never runs where it
// was enqueued - it runs wherever the game thread next drains that queue. One of
// those drains is opened mid-frame by a third-party editor tickable that blocks on a
// task:
//
//     FEngineLoop::Tick                          LaunchEngineLoop.cpp:5859
//       UEditorEngine::Tick
//         FTickableEditorObject::TickObjects     EditorEngine.cpp:1933
//           <editor tickable>::Tick              (UMassEntityEditorSubsystem, stock
//                                                 and enabled on UE 5.8)
//             FTaskBase::WaitWithNamedThreadsSupport   TaskPrivate.cpp:230-241
//               TryWaitOnNamedThread                   TaskPrivate.cpp:407-430
//                 FNamedTaskThread::ProcessTasksUntilQuit  TaskGraph.cpp:685
//                   -> a queued PinWright request runs HERE, inside the frame
//
// That is BEFORE the editor world tick (EditorEngine.cpp:1967), so no world has
// `bInTick` set and IsAnyWorldTicking() reports false while a handler runs inside the
// engine's own frame. Three editor kills in one day took that stack, two of them with
// a verb that is in the tick-unsafe table below - i.e. the gate reported "safe" on the
// one stack it exists to refuse.
//
// THE SIGNAL. FNamedTaskThread::ProcessTasksUntilQuit / ProcessTasksUntilIdle raise a
// per-queue RecursionGuard for exactly the duration of the pump (TaskGraph.cpp:690,
// :705), FNamedTaskThread::IsProcessingTasks returns it (:872-875), and
// FTaskGraphInterface::IsThreadProcessingTasks surfaces it (:1423-1429, declared
// TaskGraphInterfaces.h:333). The engine trusts the same flag for the same purpose:
// TryWaitOnNamedThread refuses to open a nested pump when one is already running
// (TaskPrivate.cpp:412). The "only a guess" caveat on the declaration is about asking
// after some OTHER thread; asked about the calling thread it is exact, which is why
// this is guarded on IsInGameThread().
//
// WHY THIS DOES NOT PING-PONG. The deferral target is the core ticker, and
// FEngineLoop::Tick calls FTSTicker::GetCoreTicker().Tick() directly
// (LaunchEngineLoop.cpp:6103) rather than through a task - no queue is being pumped
// there, so this predicate is false and UPinWrightSubsystem::Tick's drain
// (PinWrightSubsystem.cpp:480) always sees a safe point. One hop, then run. The
// automation controller and worker are ticked the same way (LaunchEngineLoop.cpp:6031,
// :6043), which is why an automation stack stays safe and the inline-path tests keep
// their meaning. That last point rests on FAutomationWorkerModule building its message
// endpoint `.WithInbox()` and draining it from its own Tick
// (AutomationWorkerModule.cpp:98, :139-150), so a test body runs on the frame's stack
// and not out of a message-delivery task.
//
// Both queues are checked. Queue 0 (ENamedThreads::GameThread) is where
// AsyncTask(ENamedThreads::GameThread, ...) lands and is the queue the stack above
// pumps; queue 1 (GameThread_Local) is pumped by FlushRenderingCommands
// (RenderingThread.cpp:1292-1293), which is a nested position by construction.
inline bool IsInsideNamedThreadPump()
{
    if (!IsInGameThread() || !FTaskGraphInterface::IsRunning())
    {
        return false;
    }
    FTaskGraphInterface& TaskGraph = FTaskGraphInterface::Get();
    return TaskGraph.IsThreadProcessingTasks(ENamedThreads::GameThread)
        || TaskGraph.IsThreadProcessingTasks(ENamedThreads::GameThread_Local);
}

// Whether tick-unsafe work may run on THIS stack.
//
// Two independent halves of one window, because neither predicate can see the other's:
// IsAnyWorldTicking() covers UWorld::Tick (including the tick-group waits that pump
// the game thread's task queue from inside it), and IsInsideNamedThreadPump() covers
// every other named-thread drain - above all the nested one an editor tickable opens
// before any world has started ticking.
inline bool IsSafeNow()
{
    return !Detail::ForcedUnsafeForTests()
        && !IsAnyWorldTicking()
        && !IsInsideNamedThreadPump();
}

// Tests only - forces IsSafeNow() to report unsafe so the deferred branch can be
// driven without standing up a real ticking world.
inline void SetForcedUnsafeForTests(bool bForced)
{
    Detail::ForcedUnsafeForTests() = bForced;
}

// Tests only - adds one method name to the tick-unsafe set. Pass an empty string
// to clear. Scoped by an RAII guard at every call site (the latch is a
// process-global, exactly like the forced-unsafe one).
inline void SetExtraTickUnsafeMethodForTests(const FString& Method)
{
    Detail::ExtraTickUnsafeMethodForTests() = Method;
}

// Park Work for a later core-ticker callback, where the tick-unsafe operations
// above are legal. RunAtSafePoint gates on IsSafeNow() first, while explicit
// continuations may deliberately take this hop from an already-safe stack.
//
// This does NOT re-check IsSafeNow(): the core ticker is proven above to run
// outside UWorld::Tick, and a re-check that could defer again would risk an
// unbounded ping-pong under a test override or an unforeseen nested pump. One
// hop, then run.
//
// The ticker callback runs outside FRpcDispatcher's FScopedUnattendedRpc, which
// only spans the synchronous handler body - the documented gap in
// Dispatch/ScopedUnattendedRpc.h ("a handler that adds a deferred continuation
// ... must hold its own PinWrightAutomationMode::Enter()/Leave() pair"). Map_Load
// and the asset-editor opens the capture verbs perform can raise engine dialogs,
// so the scope is re-entered here.
//
// Reason is optional so the pre-existing one-argument call shape (and the
// committed TestLevelLoadSafePoint.cpp assertions) keep compiling unchanged.
inline void DeferToSafePoint(TFunction<void()> Work, const TCHAR* Reason = nullptr)
{
    UE_LOG(LogPinWrightSafePoint, Log,
           TEXT("Deferring work by one safe-point core-ticker hop (%s)."),
           Reason ? Reason : TEXT("caller did not name itself"));

    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([Work](float) -> bool
        {
            FScopedUnattendedRpc UnattendedScope;
            Work();
            return false;  // one-shot
        }),
        0.0f);
}

namespace Detail
{
    inline void DeferRetainingActiveRequestToSafePoint(
        const FHandlerContext& Ctx, const TCHAR* Reason, TFunction<void()> Work,
        TFunction<void()> OnOwnerAbandoned = TFunction<void()>())
    {
        if (!Ctx.DeferActiveRequestToSafePoint(Work, Reason, OnOwnerAbandoned))
        {
            DeferToSafePoint(MoveTemp(Work), Reason);
        }
    }
}

// Always park a job body for a later core-ticker callback while preserving the
// dispatcher's active-request scope. Unlike RunAtSafePoint, this never executes
// inline and has no response facade: for non-streaming requests, StartJob has
// already sent the immediate running-ticket response; streaming requests wait
// for terminal completion instead. Work completes the job through its
// FJobOnComplete callback. Reserving the dispatcher continuation synchronously is
// essential because StartJob invokes BindNativeDelegate before the handler returns.
// Standalone contexts have no dispatcher to retain and use the same one-hop
// core-ticker path directly. No async response token is created.
inline void DeferJobToSafePoint(const FHandlerContext& Ctx, const TCHAR* Reason,
                                TFunction<void()> Work,
                                TFunction<void()> OnOwnerAbandoned = TFunction<void()>())
{
    // Job continuations (level.save/save_as and similar handlers) do not use an
    // async response token, but they still retain the request payload. Re-run
    // expectWorld immediately before their body so a world swap during the job
    // queue window cannot reach the mutator.
    FHandlerContext RetainedContext = Ctx;
    const TSharedPtr<FJsonObject> RetainedParams = Ctx.GetRawPayload();
    TFunction<void()> GuardedWork = [RetainedContext, RetainedParams, Work]() mutable
    {
        if (RetainedParams.IsValid() &&
            RetainedParams->HasField(PinWrightWorldPrecondition::ParamName) &&
            !PinWrightWorldPrecondition::Validate(RetainedContext, RetainedParams))
        {
            return;
        }
        Work();
    };
    Detail::DeferRetainingActiveRequestToSafePoint(
        Ctx, Reason, MoveTemp(GuardedWork), MoveTemp(OnOwnerAbandoned));
}

// ---------------------------------------------------------------------------
// Method-level gate (the dispatcher route)
// ---------------------------------------------------------------------------

// True for a registered RPC method whose handler body must not run inside
// UWorld::Tick. FRpcDispatcher::ProcessRequest consults this and re-queues such a
// request onto its PendingQueue, which UPinWrightSubsystem::Tick drains from the
// core ticker (PinWrightSubsystem.cpp:303) - i.e. from a proven safe point.
//
// This route exists because the alternative - hand-writing the IsSafeNow() /
// DeferToSafePoint split into every victim handler - is precisely what let four
// level verbs and the entire capture family ship ungated. Adding a verb here is a
// one-line change with no response-path surgery: the handler runs later on a safe
// stack with its ORIGINAL FHandlerContext, so text formatters, job tickets and
// test captures all behave exactly as before.
//
// The one thing it cannot cover is a handler reached through
// FRpcDispatcher::DispatchMethod (handler-to-handler cross dispatch), which
// bypasses ProcessRequest entirely. Two verbs have cross-dispatch callers, and
// they are handled differently on purpose:
//   - level.load is deliberately ABSENT from the table and carries the in-handler
//     RunAtSafePoint gate below instead, because its cross-dispatch callers
//     (editor.open_level / editor.open_asset) are the normal way it is reached.
//     A table entry would not fire for them.
//   - system.console_command IS in the table even though LevelHandler.cpp:545,
//     :630 and :687 reach it through DispatchMethod, because all three of those
//     call sites are already at a safe point by the time they forward (:545/:630
//     are inside the gated level.create; :687 is level.stream's PIE-only
//     StreamLevel). Adding an in-handler gate would give one operation two.
// The rule is not "table vs in-handler by taste": use the table unless a
// cross-dispatch caller can arrive on an ungated stack.
PINWRIGHT_API bool IsTickUnsafeMethod(const FString& Method);

// The table itself, sorted, for tests and diagnostics.
PINWRIGHT_API const TArray<FString>& GetTickUnsafeMethods();

// ---------------------------------------------------------------------------
// In-handler gate (the RunAtSafePoint route)
// ---------------------------------------------------------------------------

// The responder a RunAtSafePoint body answers through. It is the caller's own
// FHandlerContext on the inline path and an FAsyncResponseToken on the deferred
// path, because the two are NOT interchangeable: FHandlerContext::MakeAsyncToken
// deliberately drops the raw ResponseCapture pointer (HandlerContext.cpp:532-538)
// since a stack-owned capture would dangle behind a late lambda. Answering the
// inline path through a token would therefore silently bypass the synchronous
// text-formatter capture and every raw-pointer test fixture. Hence the facade
// rather than "just always use a token".
class FSafePointResponder
{
public:
    explicit FSafePointResponder(const FHandlerContext& InCtx)
        : Ctx(&InCtx)
    {
    }

    explicit FSafePointResponder(const TSharedRef<FAsyncResponseToken>& InToken)
        : Token(InToken)
    {
    }

    // True when this body is running from the core ticker rather than the
    // caller's stack. Handlers that disclose the hop in their response (see
    // level.load's `deferredToSafePoint`) read it from here.
    bool IsDeferred() const { return Ctx == nullptr; }

    void SendSuccess(const TSharedPtr<FJsonObject>& Result) const
    {
        if (Ctx) { Ctx->SendSuccess(Result); } else { Token->SendSuccess(Result); }
    }

    // Mirrors FHandlerContext::SendSuccess(const FString&), which FAsyncResponseToken
    // has no equivalent for: build the {message} object here so both paths agree.
    void SendSuccess(const FString& Message) const
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("message"), Message);
        SendSuccess(Result);
    }

    void SendSuccess(const FString& Message, const TSharedPtr<FJsonObject>& Result) const
    {
        if (Ctx) { Ctx->SendSuccess(Message, Result); } else { Token->SendSuccess(Message, Result); }
    }

    void SendError(const FString& ErrorCode, const FString& Message) const
    {
        if (Ctx) { Ctx->SendError(ErrorCode, Message); } else { Token->SendError(ErrorCode, Message); }
    }

    void SendError(const FString& ErrorCode, const FString& Message,
                   const TSharedPtr<FJsonObject>& Result) const
    {
        if (Ctx) { Ctx->SendError(ErrorCode, Message, Result); }
        else { Token->SendError(ErrorCode, Message, Result); }
    }

private:
    // Exactly one of these is set. Ctx is a bare pointer because the inline path
    // never outlives the caller's stack frame by construction.
    const FHandlerContext* Ctx = nullptr;
    TSharedPtr<FAsyncResponseToken> Token;
};

// Always defer a response-producing continuation by one core-ticker hop. The
// async token keeps response routing valid after the handler returns, while the
// retained dispatcher continuation keeps the originating request serialized
// until Work finishes. DeferToSafePoint supplies the unattended scope. A
// standalone context has no dispatcher guard to retain and uses the same
// token-backed one-hop fallback.
inline bool DeferRequestToSafePoint(
    const FHandlerContext& Ctx, const TCHAR* Reason,
    TFunction<void(const FSafePointResponder&)> Work)
{
    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
    TFunction<void()> DeferredWork = [Token, Work]()
    {
        // The dispatcher validates before it can know whether this stack will
        // defer. Re-run the same retained request contract immediately before
        // the continuation body, after any intervening world transition.
        if (Token->Payload.IsValid() &&
            Token->Payload->HasField(PinWrightWorldPrecondition::ParamName) &&
            !PinWrightWorldPrecondition::Validate(*Token, Token->Payload))
        {
            return;
        }
        Work(FSafePointResponder(Token));
    };
    Detail::DeferRetainingActiveRequestToSafePoint(
        Ctx, Reason, MoveTemp(DeferredWork));
    return true;
}

// Run Work now if this stack is outside UWorld::Tick, otherwise one core-ticker
// hop later. Always returns true, so a handler tail-calls it:
//
//     return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("level.load"),
//         [Captured...](const PinWrightSafePoint::FSafePointResponder& Responder)
//         {
//             ... tick-unsafe work ...
//             Responder.SendSuccess(Result);
//         });
//
// Everything the body needs must be captured BY VALUE: on the deferred path the
// caller's FHandlerContext, its payload pointer and every local are gone by the
// time Work runs. Capture UObject pointers as TWeakObjectPtr and re-validate.
//
// Work MUST respond through the Responder on every exit path. Nothing re-checks
// that, exactly as nothing re-checks a handler that forgets Ctx.SendSuccess.
inline bool RunAtSafePoint(const FHandlerContext& Ctx, const TCHAR* Reason,
                           TFunction<void(const FSafePointResponder&)> Work)
{
    if (IsSafeNow())
    {
        // Unchanged path for every caller that was already safe (the overwhelming
        // majority). Responding through Ctx rather than an async token keeps the
        // capture route - test fixtures, text formatters - byte-identical to the
        // pre-gate behaviour.
        Work(FSafePointResponder(Ctx));
        return true;
    }

    // Unsafe stack: the response has to outlive this handler invocation, so it goes
    // through the shareable late-response handle instead of the stack-local context.
    // When this context came from ProcessRequest, keep that dispatcher's active
    // request guard held until the continuation completes. Standalone test contexts
    // have no dispatcher and use the original direct ticker route.
    return DeferRequestToSafePoint(Ctx, Reason, MoveTemp(Work));
}
}
