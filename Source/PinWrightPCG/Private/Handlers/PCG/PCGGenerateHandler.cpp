// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Compat/EngineVersionCompat.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/PCG/PCGGenerateReadback.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Utils/ActorUtils.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"

#include "PCGCommon.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

// Per-request state for the pcg.generate job, shared (TSharedRef) by the two PCG
// completion delegates and the watchdog ticker. A named namespace (never anonymous)
// keeps it ODR-safe when unity merges TUs.
namespace PinWrightPCG
{
namespace GenerateJob
{
    struct FState
    {
        // Empty until Ctx.StartJob has allocated the ticket. An outcome produced
        // before that (nothing scheduled, or a synchronous abort inside the trigger)
        // is parked in the Deferred* fields and answered inline instead.
        FString TicketId;
        bool bResolved = false;
        bool bDeferredOutcome = false;
        bool bDeferredSuccess = false;
        FString DeferredCode;
        FString DeferredMessage;
        TSharedPtr<FJsonObject> DeferredResult;

        TWeakObjectPtr<UPCGComponent> WeakComp;
        FDelegateHandle GeneratedHandle;
        FDelegateHandle CancelledHandle;

        double StartSeconds = 0.0;
        double LastProgressSeconds = 0.0;
        // Consecutive watchdog ticks that observed !IsGenerating(); see the
        // WatchdogQuietTicks comment below.
        int32 QuietTicks = 0;
    };

    // Watchdog cadence. 1s is fine-grained enough to notice a missed completion
    // broadcast quickly without adding measurable tick cost.
    constexpr float WatchdogIntervalSeconds = 1.0f;

    // UPCGComponent::PostProcessGraph clears CurrentGenerationTask (making
    // IsGenerating() false) BEFORE it broadcasts OnPCGGraphGeneratedDelegate, both on
    // the same stack — so the delegate always wins over the watchdog in the normal
    // case. Requiring two consecutive quiet observations makes the state-poll fallback
    // fire only when the broadcast genuinely never arrived.
    constexpr int32 WatchdogQuietTicks = 2;

    // Progress cadence (bypasses the registry rate limit, like the level-build
    // heartbeat) so a streaming client sees liveness and a polling client sees the
    // elapsed time between start and completion.
    constexpr double ProgressIntervalSeconds = 5.0;
}
}

// ---- pcg.generate ----
// Trigger PCG generation on a placed actor's UPCGComponent and read back the point
// count.
//
// This is a TICKETED ASYNC JOB (Ctx.StartJob), not a held-open request. Why it must be:
//
//   * PCG generation is executed by the graph executor from UPCGSubsystem::Tick ->
//     IPCGBaseSubsystem::Tick() under a per-frame time budget, i.e. it only advances
//     while the GAME THREAD keeps ticking, over an unbounded number of frames. Any
//     in-handler wait would therefore be a hard self-deadlock (handlers run on the game
//     thread), and even a non-blocking hold is unbounded in wall-clock time: the same
//     graph that finishes in seconds on a focused editor takes minutes on a background /
//     throttled one, which is the normal state for an agent-driven session.
//   * Completion is signalled only from UPCGComponent::PostProcessGraph ->
//     OnPCGGraphGeneratedDelegate (or OnPCGGraphCancelledDelegate on abort).
//
// The previous implementation resolved a single FAsyncResponseToken from those
// delegates, which kept the HTTP request open with no ticket. When the generation
// outran the transport deadline the request was failed with TIMEOUT while the
// generation went on to succeed, and the result was unrecoverable (no ticket to poll,
// the late token resolving into a dead request id) — B-pcg-generate-deadlocks-game-thread.
// With a ticket the transport deadline can no longer destroy the answer: the ticket
// outlives the request (TTL 3600s) and carries the real terminal outcome.
//
// Completion has three independent paths, all funnelled through Resolve() so the ticket
// is completed exactly once:
//   1. OnPCGGraphGeneratedDelegate -> success with the readback.
//   2. OnPCGGraphCancelledDelegate (5.4+) -> PCG_GENERATION_CANCELLED.
//   3. The watchdog ticker -> state poll (the component stopped generating without a
//      broadcast), component destroyed, or the caller's timeout cap.
// Nothing scheduled (GenerateInternal returning InvalidPCGTaskId, which broadcasts
// nothing at all) is answered synchronously, before any ticket exists, so a caller is
// never handed a ticket for work that never started.
REGISTER_RPC_HANDLER("pcg.generate", "pcg",
    "Trigger PCG generation on a placed actor's UPCGComponent (adding one if absent, optionally assigning a graph) and read back what it produced. Ticketed async job: the kickoff returns {status:'running', ticket_id} immediately and the generation runs on the PCG scheduler; poll system.job_status for the result. The result reports instanceCount (ISM/HISM instances actually spawned) and spawnedActorCount, plus instancesByMesh rows attributing instanceCount per static mesh (instanceCount alone is a sum across species and cannot detect a mesh-entry or weight change); pointCount is reported only when the graph wires data into its Output node, and is omitted otherwise (graphOutputAvailable says which).",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Display label, name, or path of a placed actor to generate on. Accepts the actorPath/objectPath a spawn returns.")),
        RPC_PARAM_OPT("graphPath", "path", "UPCGGraph asset to assign to the actor's PCG component before generating (e.g. /Game/PCG/MyGraph). Omit to use the component's existing graph."),
        RPC_PARAM_DEF("force", "bool", "Force generation even when the component is not dirty.", "true"),
        RPC_PARAM_DEF("cleanupFirst", "bool", "Schedule a full cleanup (CleanupLocal with bRemoveComponents=true) before generating, so the pass starts from an empty slate instead of reusing previously generated components. Use after editing the graph when a re-generate looks partially inert.", "false"),
        RPC_PARAM_DEF("timeoutSeconds", "number", "How long the JOB waits for the PCG scheduler before completing the ticket as PCG_GENERATION_TIMEOUT. The generation itself is never cancelled by this — it keeps running. Clamped to 5..21600.", "1800")
    ))
{
    using namespace PinWrightPCG::GenerateJob;

    FString ActorName;
    if (!ActorNameParamUtils::RequireActorName(Ctx, ActorName)) return true;

    AActor* Actor = McpActorUtils::FindActorByName(nullptr, ActorName);
    if (!Actor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("No placed actor matched '%s' (matched by label, name, or path)."), *ActorName));
        return true;
    }

    // Optional graph to assign before generating.
    UPCGGraph* Graph = nullptr;
    const FString GraphPath = Ctx.GetString(TEXT("graphPath"));
    if (!GraphPath.IsEmpty())
    {
        Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
        if (!Graph)
        {
            Ctx.SendError(TEXT("GRAPH_NOT_FOUND"),
                FString::Printf(TEXT("Could not load a UPCGGraph at '%s'."), *GraphPath));
            return true;
        }
    }

    const bool bForce = Ctx.GetBool(TEXT("force"), true);
    const bool bCleanupFirst = Ctx.GetBool(TEXT("cleanupFirst"), false);
    const double TimeoutSeconds =
        FMath::Clamp(Ctx.GetNumber(TEXT("timeoutSeconds"), 1800.0), 5.0, 21600.0);

    // Resolve or add the actor's PCG component, then assign the graph. Both are
    // undoable editor mutations, so they run inside a transaction; get-or-add keeps
    // the handler idempotent under client retry (agent-conventions.md).
    UPCGComponent* PcgComp = Actor->FindComponentByClass<UPCGComponent>();
    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWright", "PcgGenerate", "PCG Generate"));
        if (!PcgComp)
        {
            Actor->Modify();
            PcgComp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(),
                NAME_None, RF_Transactional);
            if (!PcgComp)
            {
                Ctx.SendError(TEXT("CREATE_COMPONENT_FAILED"),
                    FString::Printf(TEXT("Failed to add a UPCGComponent to '%s'."), *Actor->GetActorLabel()));
                return true;
            }
            Actor->AddInstanceComponent(PcgComp);
            PcgComp->OnComponentCreated();
            PcgComp->RegisterComponent();
        }

        if (Graph)
        {
            PcgComp->Modify();
            PcgComp->SetGraphLocal(Graph);
        }
    }

    // Nothing to generate without a graph.
    UPCGGraph* EffectiveGraph = PcgComp->GetGraph();
    if (!EffectiveGraph)
    {
        Ctx.SendError(TEXT("NO_PCG_GRAPH"),
            FString::Printf(TEXT("Actor '%s' has a PCG component but no graph assigned; pass graphPath to assign one."),
                *Actor->GetActorLabel()));
        return true;
    }

    const FString ActorLabel = Actor->GetActorLabel();
    const FString GraphName = EffectiveGraph->GetName();
    const FString GraphAssetPath = EffectiveGraph->GetPathName();

    // Opt-in full cleanup. PCG's own pre-pass (UPCGComponent::CreateGenerateTask) only
    // does CleanupLocal(bRemoveComponents=false), which keeps previously generated
    // components around for reuse — the reason an edited graph can look partially inert
    // on a re-generate. CleanupLocal assigns CurrentCleanupTask synchronously, so the
    // generate task scheduled below picks it up as a dependency and runs after it.
    if (bCleanupFirst)
    {
        // The one-argument form is the 5.6+ API; before that the second `bSave`
        // parameter was part of the signature (it is a deprecated passthrough on 5.6+,
        // so calling it there would warn).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        PcgComp->CleanupLocal(/*bRemoveComponents=*/true);
#else
        PcgComp->CleanupLocal(/*bRemoveComponents=*/true, /*bSave=*/false);
#endif
    }

    TSharedRef<FState> State = MakeShared<FState>();
    State->WeakComp = PcgComp;
    State->StartSeconds = FPlatformTime::Seconds();

    // Reads back what the generation left behind. Three DIFFERENT numbers live here and
    // the payload never lets them be confused (B-pcg-generated-graph-output-empty-after-generate):
    //
    //   pointCount   — points that reached the GRAPH'S OUTPUT NODE. Omitted entirely when
    //                  the graph output is empty, because a graph that spawns through a
    //                  StaticMeshSpawner without wiring its Out pin to the graph Output
    //                  node legitimately emits nothing while spawning thousands of
    //                  instances. Reporting 0 there is a confident wrong answer, and a
    //                  caller gating on `pointCount > 0` would call every such success a
    //                  failure. `graphOutputAvailable` is always present so the omission
    //                  is detectable rather than looking like a dropped field.
    //   instanceCount — ISM/HISM instances PCG actually spawned and still manages. This is
    //                  the number that matched the 97 / 64 observed live.
    //   spawnedActorCount — actors PCG spawned and still manages.
    //
    // instanceCount is a SUM across species, so it is blind to a mesh-entry or weight edit
    // (a weighted selector partitions a fixed point set; the sum is the sampler's number).
    // instancesByMesh attributes it per static mesh so the response says WHAT was spawned,
    // not only how much (B-pcg-generate-instancecount-blind-to-species).
    //
    // All three are structurally zero for a graph whose product is neither points nor managed
    // resources (an Export node, an attribute set, any plugin-defined spatial type), so
    // dataTypes[] enumerates the output BY CLASS and pointCount is omitted whenever
    // pointDataCount is 0 — otherwise a graph that worked and one that did nothing return the
    // same confident zeros (B-pcg-generate-blind-to-non-point-data).
    //
    // See PCGGenerateReadback.h for the engine-source evidence behind each.
    auto BuildResult = [ActorLabel, GraphName, GraphAssetPath, State]
        (UPCGComponent* Comp, const TCHAR* CompletionSignal) -> TSharedPtr<FJsonObject>
    {
        return PinWrightPCG::BuildGenerationPayload(
            PinWrightPCG::ReadGeneration(Comp),
            ActorLabel, GraphName, GraphAssetPath, CompletionSignal,
            FMath::RoundToInt(FPlatformTime::Seconds() - State->StartSeconds));
    };

    // Whichever completion path resolves first drops BOTH bindings so the other never
    // lingers on the component (and can't re-resolve behind the bResolved flag).
    auto Unbind = [State]()
    {
        if (UPCGComponent* Comp = State->WeakComp.Get())
        {
            Comp->OnPCGGraphGeneratedDelegate.Remove(State->GeneratedHandle);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            Comp->OnPCGGraphCancelledDelegate.Remove(State->CancelledHandle);
#endif
        }
    };

    // Single funnel for every outcome. Before Ctx.StartJob has allocated a ticket the
    // outcome is parked for the inline reply below; after it, the outcome completes the
    // ticket (FJobRegistry::Complete is itself a no-op once the ticket left "running",
    // so a late delegate cannot overwrite a cancelled/completed ticket).
    auto Resolve = [State, Unbind](bool bSuccess, const TSharedPtr<FJsonObject>& Result,
                                   const FString& ErrorCode, const FString& Message)
    {
        if (State->bResolved)
        {
            return;
        }
        State->bResolved = true;
        Unbind();

        if (State->TicketId.IsEmpty())
        {
            State->bDeferredOutcome = true;
            State->bDeferredSuccess = bSuccess;
            State->DeferredResult = Result;
            State->DeferredCode = ErrorCode;
            State->DeferredMessage = Message;
            return;
        }

        TSharedPtr<FJsonObject> Payload = Result;
        if (!bSuccess)
        {
            // The typed code travels in the ticket's `error` field (house convention);
            // the human-readable detail rides along in the result payload so a poller
            // gets both from one system.job_status call.
            if (!Payload.IsValid())
            {
                Payload = MakeShared<FJsonObject>();
            }
            Payload->SetStringField(TEXT("message"), Message);
        }
        FPluginState::Get().GetJobRegistry().Complete(
            State->TicketId, bSuccess, Payload, bSuccess ? FString() : ErrorCode);
    };

    State->GeneratedHandle = PcgComp->OnPCGGraphGeneratedDelegate.AddLambda(
        [State, Resolve, BuildResult](UPCGComponent* /*CompletedComp*/)
        {
            if (State->bResolved)
            {
                // The generated delegate is multicast and can fire once per
                // hierarchical-grid pass; only the first one carries the answer.
                return;
            }
            Resolve(true, BuildResult(State->WeakComp.Get(), TEXT("generated_delegate")),
                FString(), FString());
        });

    // UPCGComponent::OnPCGGraphCancelledDelegate arrived in UE 5.4. On 5.3 an aborted pass
    // has no broadcast at all, so there is nothing to bind: the generated delegate, the
    // InvalidPCGTaskId synchronous resolve, and the watchdog remain the completion paths.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    State->CancelledHandle = PcgComp->OnPCGGraphCancelledDelegate.AddLambda(
        [State, Resolve, ActorLabel](UPCGComponent* /*CancelledComp*/)
        {
            Resolve(false, nullptr, TEXT("PCG_GENERATION_CANCELLED"),
                FString::Printf(TEXT("PCG generation on '%s' was cancelled or aborted before completing."),
                    *ActorLabel));
        });
#endif

    // Trigger generation and capture the scheduled task id. InvalidPCGTaskId means
    // nothing was scheduled, so neither completion delegate will ever fire — resolve now
    // instead of allocating a ticket that could never move: read back existing output if
    // the component was already generated (an idempotent force=false re-request),
    // otherwise report that generation didn't schedule. Resolve() no-ops if a synchronous
    // abort inside the trigger already fired the cancelled delegate.
    const FPCGTaskId TaskId = PcgComp->GenerateLocalGetTaskId(bForce);
    if (TaskId == InvalidPCGTaskId)
    {
        // "Was anything produced?" must NOT be `GetGeneratedGraphOutput().TaggedData.Num() > 0`.
        // That accessor is empty for any graph that spawns without wiring its Out pin to the
        // graph Output node, so gating on it told callers "nothing was generated and no prior
        // output exists" while 64 instances sat on the actor — the second symptom in
        // B-pcg-generated-graph-output-empty-after-generate. Count what survives instead, and
        // treat the component's own bGenerated flag as authoritative for an empty-but-completed
        // pass.
        const PinWrightPCG::FGenerationReadback Readback = PinWrightPCG::ReadGeneration(PcgComp);
        if (PinWrightPCG::HasProducedOutput(Readback) || PcgComp->bGenerated)
        {
            Resolve(true, BuildResult(PcgComp, TEXT("already_generated")), FString(), FString());
        }
        else
        {
            Resolve(false, nullptr, TEXT("PCG_GENERATION_NOT_SCHEDULED"),
                FString::Printf(TEXT("PCG generation did not schedule on '%s' (component deactivated, no PCG subsystem, or managed by the runtime generation scheduler), and the component reports no prior generation and no surviving generated resources."),
                    *ActorLabel));
        }
    }

    // Anything already resolved on this stack is answered inline — the caller gets the
    // same synchronous typed error / readback it always did, and no ticket is created.
    if (State->bDeferredOutcome)
    {
        if (State->bDeferredSuccess)
        {
            Ctx.SendSuccess(State->DeferredResult);
        }
        else
        {
            Ctx.SendError(State->DeferredCode, State->DeferredMessage);
        }
        return true;
    }

    // Scheduled: hand back a ticket immediately (or, for a streaming request, register
    // the stream bridge that resolves on the terminal job event) and let the delegates /
    // watchdog complete it.
    FJobBindArgs Args;
    Args.Method = TEXT("pcg.generate");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("actor"), ActorLabel);
    Args.StartedPayload->SetStringField(TEXT("graph"), GraphName);
    Args.StartedPayload->SetStringField(TEXT("graphPath"), GraphAssetPath);
    Args.StartedPayload->SetBoolField(TEXT("partitioned"), PcgComp->IsPartitioned());
    Args.StartedPayload->SetStringField(TEXT("note"),
        TEXT("Generation runs on the PCG scheduler over many game-thread frames. Poll system.job_status with this ticket_id until the status is terminal; the produced counts arrive in the completion result."));
    // Completion is driven by the PCG delegates + the watchdog ticker below, so there is
    // no native completion delegate to bind here (same shape as asset.dump_folder).
    Args.BindNativeDelegate = [](FJobOnComplete /*OnComplete*/) {};

    State->TicketId = Ctx.StartJob(Args);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // system.job_cancel -> stop the PCG pass. UPCGComponent::CancelGeneration routes to
    // UPCGSubsystem::CancelGeneration, which aborts the component and (5.4+) broadcasts
    // OnPCGGraphCancelledDelegate; on 5.3 the watchdog's state poll closes the ticket
    // instead, which is why this is gated on the same seam as that delegate.
    FPluginState::Get().GetJobRegistry().SetCancelCallback(State->TicketId,
        [WeakComp = State->WeakComp]()
        {
            if (UPCGComponent* Comp = WeakComp.Get())
            {
                Comp->CancelGeneration();
            }
        });
#endif

    // Watchdog. Its job is that the ticket ALWAYS reaches a terminal state, even if the
    // component's completion broadcast never arrives (component destroyed mid-pass, an
    // abort path with no delegate on 5.3, a future engine change). It never cancels the
    // generation itself — a timeout completes the ticket and says so, leaving the pass
    // running, so a caller can re-read the point count later instead of being told the
    // work failed.
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [State, Resolve, Unbind, BuildResult, ActorLabel, TimeoutSeconds](float) -> bool
    {
        if (State->bResolved)
        {
            return false;
        }

        const double Elapsed = FPlatformTime::Seconds() - State->StartSeconds;
        UPCGComponent* Comp = State->WeakComp.Get();
        if (!Comp || !IsValid(Comp))
        {
            Resolve(false, nullptr, TEXT("PCG_COMPONENT_GONE"),
                FString::Printf(TEXT("The PCG component on '%s' was destroyed while its generation was in flight; the outcome is unknown."),
                    *ActorLabel));
            return false;
        }

        if (!Comp->IsGenerating())
        {
            if (++State->QuietTicks >= WatchdogQuietTicks)
            {
                // The pass ended without a broadcast we saw. Read the component's own
                // state rather than assuming failure: surviving generated resources
                // (or graph output, or bGenerated, or a partitioned component whose
                // output lives on its local components) mean the work SUCCEEDED and must
                // be reported as such. The resource counts are what make this reliable —
                // the graph-output check alone reads "no output" for any spawner graph.
                const bool bProduced = PinWrightPCG::HasProducedOutput(PinWrightPCG::ReadGeneration(Comp))
                    || Comp->bGenerated
                    || Comp->IsPartitioned();
                TSharedPtr<FJsonObject> Payload = BuildResult(Comp, TEXT("state_poll"));
                if (bProduced)
                {
                    Resolve(true, Payload, FString(), FString());
                }
                else
                {
                    Resolve(false, Payload, TEXT("PCG_GENERATION_ENDED_WITHOUT_OUTPUT"),
                        FString::Printf(TEXT("PCG generation on '%s' stopped without a completion or cancellation broadcast and produced no output."),
                            *ActorLabel));
                }
                return false;
            }
        }
        else
        {
            State->QuietTicks = 0;
        }

        if (Elapsed >= TimeoutSeconds)
        {
            TSharedPtr<FJsonObject> Payload = BuildResult(Comp, TEXT("timeout"));
            Payload->SetBoolField(TEXT("stillGenerating"), Comp->IsGenerating());
            if (PinWrightPCG::HasProducedOutput(PinWrightPCG::ReadGeneration(Comp)))
            {
                // Output already landed — never fail work that succeeded. Counts what
                // survives, not just the graph-output collection, so a spawner graph that
                // finished during the timeout window is not failed for producing "nothing".
                Resolve(true, Payload, FString(), FString());
            }
            else
            {
                Resolve(false, Payload, TEXT("PCG_GENERATION_TIMEOUT"),
                    FString::Printf(TEXT("PCG generation on '%s' did not finish within %.0fs. It has NOT been cancelled and is still progressing on the PCG scheduler (which only advances while the editor ticks); re-read the counts later, e.g. with another pcg.generate using force=false."),
                        *ActorLabel, TimeoutSeconds));
            }
            return false;
        }

        if (Elapsed - State->LastProgressSeconds >= ProgressIntervalSeconds)
        {
            State->LastProgressSeconds = Elapsed;
            TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
            Progress->SetNumberField(TEXT("elapsedSeconds"), FMath::RoundToInt(Elapsed));
            Progress->SetBoolField(TEXT("generating"), Comp->IsGenerating());
            const bool bStillRunning = FPluginState::Get().GetJobRegistry().RecordProgress(
                State->TicketId,
                FString::Printf(TEXT("PCG generation on '%s' running, %ds elapsed"),
                    *ActorLabel, FMath::RoundToInt(Elapsed)),
                Progress, /*bBypassRateLimit=*/true);
            if (!bStillRunning)
            {
                // The ticket left "running" without us (system.job_cancel). Drop the
                // bindings and stop; there is nothing left to complete.
                State->bResolved = true;
                Unbind();
                return false;
            }
        }
        return true;
    }), WatchdogIntervalSeconds);

    return true;
}

#endif // __has_include("PCGGraph.h")
