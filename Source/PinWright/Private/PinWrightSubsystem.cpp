// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightSubsystem.h"

#include "PinWrightHelpers.h"

#include "IntegrationGates.h"
#include "State/PluginState.h"
#include "Transport/PortAdvertisement.h"
#include "Transport/ModalStateProbe.h"
#include "Transport/SocketHttpServer.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/WorldPrecondition.h"
#include "Catalog/ToolCatalog.h"
#include "Catalog/WikiDiskGenerator.h"
#include "PinWrightSettings.h"
#include "PinWrightProjectSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Utils/ActorUtils.h"
#include "Utils/GatewayPortFile.h"
#include "Utils/HttpResponseSpill.h"
#include "Utils/JobMonitorLog.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

#include "Editor.h"
#include "Compat/EngineVersionCompat.h"
#include "CoreGlobals.h"
#include "Selection.h"

DEFINE_LOG_CATEGORY(LogPinWrightSubsystem);

// FEditorDelegates::OnEditorInitialized is a one-shot notification. Keep a
// module-lifetime latch so a subsystem instance recreated later in the same
// editor process can catch up instead of remaining permanently unready.
static bool GPinWrightEditorInitializationObserved = false;

// Sanitize incoming text for logging: replace control characters
static inline FString SanitizeForLog(const FString& In)
{
    if (In.IsEmpty())
        return FString();
    FString Out;
    Out.Reserve(FMath::Min<int32>(In.Len(), 1024));
    for (int32 i = 0; i < In.Len(); ++i)
    {
        const TCHAR C = In[i];
        if (C >= 32 && C != 127)
            Out.AppendChar(C);
        else
            Out.AppendChar('?');
    }
    if (Out.Len() > 512)
        Out = Out.Left(512) + TEXT("[TRUNCATED]");
    return Out;
}

void UPinWrightSubsystem::Initialize(
    FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Skip initialization during commandlet execution (cooking, packaging, etc.)
    if (IsRunningCommandlet())
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWrightSubsystem skipping initialization - "
                    "running as commandlet (cook/package mode)."));
        return;
    }

    UE_LOG(LogPinWrightSubsystem, Log,
           TEXT("PinWrightSubsystem initializing."));

    // Do not treat the socket bind or a protocol ping as operational readiness.
    // The editor-initialized delegate is the startup boundary; RefreshEditorReadiness
    // below additionally requires package loading to be idle and the typed-element
    // selection set used by editor tooling to exist.
    bEditorInitialized = GPinWrightEditorInitializationObserved;

    // Migration warning: AssetDumpRootDirectory / WikiOutputDirectory moved from
    // the per-user UPinWrightSettings to the project-level UPinWrightProjectSettings.
    // If the new setting is unset but a stale per-user value remains on disk, tell
    // the user to re-set it — never rewrite config files automatically.
    {
        const UPinWrightProjectSettings* ProjectSettings =
            GetDefault<UPinWrightProjectSettings>();
        auto WarnIfStalePerUserValue =
            [](const FString& NewValue, const TCHAR* PropertyName)
        {
            if (!NewValue.IsEmpty() || !GConfig)
            {
                return;
            }
            FString StaleValue;
            if (GConfig->GetString(TEXT("/Script/PinWright.PinWrightSettings"),
                                   PropertyName, StaleValue, GEditorPerProjectIni)
                && !StaleValue.IsEmpty())
            {
                UE_LOG(LogPinWrightSubsystem, Warning,
                       TEXT("Setting %s moved to Project Settings -> Plugins -> PinWright (Project); ")
                       TEXT("the old per-user value (%s) is ignored and must be re-set there."),
                       PropertyName, *StaleValue);
            }
        };
        WarnIfStalePerUserValue(ProjectSettings->AssetDumpRootDirectory,
                                TEXT("AssetDumpRootDirectory"));
        WarnIfStalePerUserValue(ProjectSettings->WikiOutputDirectory,
                                TEXT("WikiOutputDirectory"));
    }

    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
    FString IsolatedTestChildMonitorPath;
    const bool bIsolatedTestChild = FParse::Value(
        FCommandLine::Get(), TEXT("PinWrightIsolatedTestChild="), IsolatedTestChildMonitorPath)
        && !IsolatedTestChildMonitorPath.IsEmpty();

    // Create decomposed components. The socket-based SSE-capable server is the
    // single transport.
    StreamingTransport = MakeShared<FSocketHttpServer>();
    Dispatcher = MakeShared<FRpcDispatcher>();
    Catalog = MakeShared<FToolCatalog>();
    RefreshEditorReadiness();

    // Wire up components. The dispatcher resolves ALL responses (handler
    // results and its own direct-resolve paths: unknown-action, _format:text,
    // exception catches) through this sink, which routes to the transport.
    Dispatcher->Initialize([this](const FString& RequestId, bool bSuccess, const FString& Message,
                                  const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
    {
        SendAutomationResponse(RequestId, bSuccess, Message, Result, ErrorCode);
    });

    // Load engine-plugin integration sub-modules (LoadingPhase=None) whose owning
    // plugin is enabled. Must precede DrainAutoRegistrations so sub-module
    // static-init handler registrations drain together with the main-module ones,
    // land in the startup wiki generation, and precede transport start.
    IntegrationGates::LoadEnabledIntegrations();

    // Drain auto-registered handlers (must happen before Catalog caches namespace counts)
    Dispatcher->DrainAutoRegistrations(this);

    // Drain auto-registered text formatters — companion to handler registration,
    // wired before any request can arrive so the text-format intercept path is live.
    Dispatcher->DrainFormatterRegistrations();

    Catalog->Initialize(Dispatcher);

    // Isolated test children share the project directory with the host editor.
    // Keep their startup read-only outside their private automation log directory.
    if (!bIsolatedTestChild)
    {
        WikiDiskGenerator::Generate();
    }

    // Bind the transport's request delegate to the dispatcher.
    const TWeakPtr<FRpcDispatcher> WeakDispatcher = Dispatcher;
    StreamingTransport->OnRequestReceived.BindLambda(
        [WeakDispatcher](const FString& RequestId, const FString& Method,
                         const TSharedPtr<FJsonObject>& Params)
        {
            if (const TSharedPtr<FRpcDispatcher> PinnedDispatcher =
                WeakDispatcher.Pin())
            {
                PinnedDispatcher->ProcessRequest(RequestId, Method, Params);
            }
        });

    // Wipe jobs.jsonl and all rotation segments from the previous session.
    if (!bIsolatedTestChild)
    {
        const FString JobsPath = FPaths::ProjectDir() / JobMonitorLog::JobsJsonlRelativePath;
        IFileManager& FM = IFileManager::Get();
        FM.Delete(*JobsPath, /*RequireExists=*/false, /*EvenReadOnly=*/false);
        const UPinWrightSettings* WipeSettings =
            GetDefault<UPinWrightSettings>();
        for (int32 i = 1; i <= WipeSettings->MonitorFileRotationKeep; ++i)
        {
            const FString RotPath = FString::Printf(TEXT("%s.%d"), *JobsPath, i);
            FM.Delete(*RotPath, /*RequireExists=*/false, /*EvenReadOnly=*/false);
        }
    }

    // Start HTTP transport
    if (bIsolatedTestChild)
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWright HTTP transport disabled for isolated automation child."));
    }
    else if (Settings && Settings->bEnableHttpTransport)
    {
        HttpResponseSpill::PruneOldHttpResponseSpills();
        const int32 EffectivePort = UPinWrightSettings::ResolveHttpPort(Settings);
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("HTTP transport binding port %d."), EffectivePort);
        // Socket-based transport; it reads auth/timeout settings itself, so
        // Start only takes the port. The subsystem tracks the bind outcome
        // because the server exposes no active/port accessors. A lost bind arms
        // the retry schedule below rather than ending the transport's life -
        // see TickBindRetry.
        TryStartTransport(EffectivePort, /*bIsRetry=*/false);
    }
    else
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("HTTP transport is disabled in settings."));
    }

    // Register ticker
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this,
                                       &UPinWrightSubsystem::Tick),
        0.1f);

    UE_LOG(LogPinWrightSubsystem, Log,
           TEXT("PinWrightSubsystem initialized (socket transport)."));
}

void UPinWrightSubsystem::Deinitialize()
{
    if (TickHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }

    if (!IsRunningCommandlet())
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWrightSubsystem deinitializing."));
    }

    if (JobEventHandle.IsValid())
    {
        FPluginState::Get().GetJobRegistry().OnJobEvent().Remove(JobEventHandle);
        JobEventHandle.Reset();
    }
    StreamingJobRequests.Empty();

    // Stop I/O-thread request handoffs before destroying their dispatcher owner.
    // The I/O thread remains alive to flush typed async-handler abandonment
    // responses and the generic failures for any requests left afterward.
    if (StreamingTransport.IsValid())
    {
        StreamingTransport->QuiesceRequestIntake();
    }
    Catalog.Reset();
    Dispatcher.Reset();

    if (StreamingTransport.IsValid())
    {
        StreamingTransport->FailAllCompletions(TEXT("Automation bridge shutting down."),
                                               TEXT("BRIDGE_SHUTTING_DOWN"));
        StreamingTransport->Stop();
    }

    StreamingTransport.Reset();
    bStreamingActive = false;
    StreamingPort = 0;
    ContestedPort = 0;
    BindRetry.Reset();
    bPortPublishPending = false;

    // FailAllCompletions above gives normal pending requests their terminal error
    // response.  DropCompletionsForConnection intentionally has no callback for a
    // disconnected client, so discard any remaining per-session policies at the
    // subsystem boundary rather than retaining abandoned request IDs indefinitely.
    {
        FScopeLock Lock(&ResponsePolicyMutex);
        ResponsePolicies.Empty();
    }

    Super::Deinitialize();
}

bool UPinWrightSubsystem::IsBridgeActive() const
{
    return StreamingTransport.IsValid() && bStreamingActive;
}

EPinWrightState
UPinWrightSubsystem::GetBridgeState() const
{
    return IsBridgeActive() ? EPinWrightState::Connected
                            : EPinWrightState::Disconnected;
}

int32 UPinWrightSubsystem::GetBoundHttpPort() const
{
    return (StreamingTransport.IsValid() && bStreamingActive) ? StreamingPort : 0;
}

EMcpServerStatus UPinWrightSubsystem::GetServerStatus() const
{
    if (!StreamingTransport.IsValid())
    {
        return EMcpServerStatus::Disabled;
    }
    if (bStreamingActive)
    {
        return EMcpServerStatus::Listening;
    }
    // A lost bind is PortInUse, not Disabled. PortInUse used to be unreachable, which made
    // the setup screen's red "MCP server is NOT running - port conflict" banner and the
    // module's show-setup-screen-on-problem path (PinWrightModule.cpp) dead code: the one
    // failure they exist for reported itself as "transport off in settings" instead.
    return (ContestedPort != 0) ? EMcpServerStatus::PortInUse : EMcpServerStatus::Disabled;
}

int32 UPinWrightSubsystem::GetBindAttemptCount() const
{
    return BindRetry.GetFailedAttempts();
}

bool UPinWrightSubsystem::IsBindRetryExhausted() const
{
    return BindRetry.IsExhausted();
}

bool UPinWrightSubsystem::TryStartTransport(int32 Port, bool bIsRetry)
{
    if (!StreamingTransport.IsValid())
    {
        return false;
    }

    FSocketHttpServer::EStartResult StartResult = FSocketHttpServer::EStartResult::Unavailable;
    bStreamingActive = StreamingTransport->Start(static_cast<uint32>(Port), &StartResult);

    if (bStreamingActive)
    {
        StreamingPort = Port;
        ContestedPort = 0;
        BindRetry.NoteSucceeded();

        // Publish the bound port for the stdio proxy, which re-reads the file before every
        // call. Written HERE rather than only at startup, because a late bind that skipped it
        // would serve HTTP on a port no proxy config points at.
        PublishBoundPort(Port);

        // Bridge job-registry events onto open SSE streams. Guarded: a retry must not
        // subscribe a second time and double every job event.
        if (!JobEventHandle.IsValid())
        {
            JobEventHandle = FPluginState::Get().GetJobRegistry().OnJobEvent()
                .AddUObject(this, &UPinWrightSubsystem::HandleJobEvent);
        }

        if (bIsRetry)
        {
            UE_LOG(LogPinWrightSubsystem, Log,
                   TEXT("Transport bound port %d on retry %d - MCP is serving again."),
                   Port, BindRetry.GetFailedAttempts());
        }
        return true;
    }

    // Not bound. Only a port collision can clear itself by waiting; a missing socket
    // subsystem or a failed I/O thread cannot, so those are terminal immediately rather
    // than burning a ten-minute budget on a retry that provably cannot work.
    if (StartResult == FSocketHttpServer::EStartResult::PortInUse)
    {
        ContestedPort = Port;
        if (!bIsRetry)
        {
            BindRetry.Begin(FPlatformTime::Seconds(), PinWrightBindRetry::FPolicy());
            const PinWrightBindRetry::FPolicy& Policy = BindRetry.GetPolicy();
            UE_LOG(LogPinWrightSubsystem, Error,
                   TEXT("Transport failed to bind port %d; retrying for up to %.0fs "
                        "(backoff %.0fs doubling to %.0fs). This editor is NOT serving MCP "
                        "until it binds."),
                   Port, Policy.TotalBudgetSeconds, Policy.FirstDelaySeconds,
                   Policy.MaxDelaySeconds);

            // The advertisement is wrong the moment we know we are not serving, not ten
            // minutes later: a caller resolving the endpoint during the backoff would
            // otherwise be aimed at whatever the last successful bind of any past session
            // wrote. Retracted only when nothing is listening there - see
            // Transport/PortAdvertisement.h for why liveness, not ownership, is the test.
            PortAdvertisement::ReconcileWhileNotServing();
        }
    }
    else
    {
        // Not a collision. Abandon any schedule already running rather than spending the
        // budget on a condition waiting provably cannot clear, and drop ContestedPort so the
        // status stops claiming a port conflict that is not one.
        BindRetry.Reset();
        ContestedPort = 0;
        UE_LOG(LogPinWrightSubsystem, Error,
               TEXT("Transport failed to start on port %d for a reason waiting cannot fix "
                    "(no socket subsystem, or the I/O thread did not start). Not retrying."),
               Port);
        PortAdvertisement::ReconcileWhileNotServing();
    }
    return false;
}

void UPinWrightSubsystem::PublishBoundPort(int32 Port)
{
    if (GatewayPortFile::WritePortFile(Port))
    {
        bPortPublishPending = false;
        return;
    }

    // Bound but undiscoverable. The write failing is rare (a read-only Saved/, a full disk,
    // an AV holding the file) and used to be discarded silently, which produced the same
    // user-visible outcome as a lost bind - "the editor is up and MCP does not answer" - with
    // nothing in the log to distinguish them. Say so once per transition, then keep trying.
    if (!bPortPublishPending)
    {
        UE_LOG(LogPinWrightSubsystem, Error,
               TEXT("Transport is serving on port %d but the port could not be published to %s. ")
               TEXT("Callers resolve the endpoint from that file, so this editor is unreachable ")
               TEXT("until the write succeeds; it will be retried every tick. Check that the ")
               TEXT("directory is writable."),
               Port, *GatewayPortFile::GetPortFilePath());
    }
    bPortPublishPending = true;
}

void UPinWrightSubsystem::TickBindRetry()
{
    // A bind that succeeded while its publication failed leaves a server nobody can find.
    // Retried here rather than at the next launch, on the same ticker the rebind uses.
    if (bPortPublishPending && bStreamingActive && StreamingPort != 0)
    {
        PublishBoundPort(StreamingPort);
    }

    if (!BindRetry.IsRetrying())
    {
        return;
    }

    const double Now = FPlatformTime::Seconds();
    switch (BindRetry.Advance(Now))
    {
    case PinWrightBindRetry::EAction::Attempt:
    {
        if (TryStartTransport(ContestedPort, /*bIsRetry=*/true))
        {
            break;
        }
        const int32 FailedPort = ContestedPort;
        if (!BindRetry.IsRetrying())
        {
            // TryStartTransport abandoned the schedule: the retry failed for a reason waiting
            // cannot fix, and it has already said so.
            break;
        }
        BindRetry.NoteAttemptFailed(Now);
        if (!BindRetry.IsExhausted())
        {
            // Per-attempt line at Warning, on the backoff cadence rather than every 0.1s
            // tick. The transport's own full remediation text stays a one-shot.
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("Port %d still held after %d attempts (%.0fs); next retry in %.0fs."),
                   FailedPort, BindRetry.GetFailedAttempts(),
                   BindRetry.GetElapsedSeconds(Now),
                   BindRetry.GetNextAttemptSeconds() - Now);
        }
        break;
    }
    case PinWrightBindRetry::EAction::Relog:
        // Repeated on the exhausted cadence, deliberately: one line at startup is invisible
        // to anyone who attaches to the log later, and "editor process alive, MCP dead" is
        // exactly the state nobody thinks to look for.
        UE_LOG(LogPinWrightSubsystem, Error,
               TEXT("PinWright MCP is NOT serving: port %d was held by another process for the "
                    "whole %.0fs retry budget (%d attempts). This editor process is alive but "
                    "unreachable and will stay that way. Fix: enable \"Auto-derive Port From "
                    "Project Path\" in Project Settings (PinWright), or set a different fixed "
                    "HttpPort, then restart this editor and re-run onboarding."),
               ContestedPort, BindRetry.GetPolicy().TotalBudgetSeconds,
               BindRetry.GetFailedAttempts());
        // Re-judged on the same cadence: the process that held the port may have exited since
        // the last look, which turns a Retain into a Retract - the file would otherwise keep
        // advertising a port that stopped being served after we gave up watching it.
        PortAdvertisement::ReconcileWhileNotServing();
        break;

    case PinWrightBindRetry::EAction::None:
    default:
        break;
    }
}

bool UPinWrightSubsystem::Tick(float DeltaTime)
{
    // The core ticker running at all proves no modal owns the game thread: a nested
    // Slate modal loop pumps only Slate, never FTSTicker. That inversion - latch from
    // inside the modal loop, clear from here - is what makes the clear trustworthy.
    ModalStateProbe::NoteGameThreadAlive();

    RefreshEditorReadiness();

    // Before the transport tick: until the bind lands there is no transport to tick.
    TickBindRetry();

    if (StreamingTransport.IsValid())
    {
        StreamingTransport->Tick(DeltaTime);
    }

    // Process deferred requests when engine state is safe
    if (Dispatcher.IsValid() && !UE::IsSavingPackage() &&
        !IsGarbageCollecting())
    {
        Dispatcher->ProcessPendingRequests();
    }

    // Evict completed tickets every tick (cheap; only walks completed entries past TTL).
    FPluginState::Get().GetJobRegistry().EvictExpired(FDateTime::UtcNow());

    return true;
}

void UPinWrightSubsystem::RecordEditorInitialized(double StartupDurationSeconds)
{
    GPinWrightEditorInitializationObserved = true;
    UE_LOG(LogPinWrightSubsystem, Log,
           TEXT("Editor initialization completed after %.2f seconds; PinWright operational readiness can now be evaluated."),
           StartupDurationSeconds);
}

void UPinWrightSubsystem::RefreshEditorReadiness()
{
    // The module owns the one-shot delegate subscription and is initialized
    // before editor subsystems. Copy its module-lifetime latch every tick so a
    // subsystem created or recreated after the broadcast catches up.
    bEditorInitialized = bEditorInitialized || GPinWrightEditorInitializationObserved;

    const bool bEditorAvailable = GEditor != nullptr;
    const bool bEditorLoadingPackage =
        !bEditorAvailable || MCP_IS_EDITOR_LOADING_PACKAGE;

    USelection* SelectedActors = bEditorAvailable
        ? GEditor->GetSelectedActors()
        : nullptr;
    const bool bEditorSelectionSetAvailable =
        SelectedActors != nullptr && SelectedActors->GetElementSelectionSet() != nullptr;

    const bool bNextReady = bEditorInitialized && bEditorAvailable &&
        !bEditorLoadingPackage && bEditorSelectionSetAvailable;
    if (bNextReady != bEditorReady)
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWright editor readiness changed: %s (initialized=%s, loadingPackage=%s, selectionSet=%s)."),
               bNextReady ? TEXT("ready") : TEXT("not ready"),
               bEditorInitialized ? TEXT("true") : TEXT("false"),
               bEditorLoadingPackage ? TEXT("true") : TEXT("false"),
               bEditorSelectionSetAvailable ? TEXT("true") : TEXT("false"));
    }
    bEditorReady = bNextReady;

    if (StreamingTransport.IsValid())
    {
        StreamingTransport->SetEditorReadiness(
            bEditorReady, bEditorLoadingPackage, bEditorSelectionSetAvailable);
    }
}

void UPinWrightSubsystem::RegisterResponsePolicy(
    const FString& RequestId, const bool bMutating, const FString& Method)
{
    if (RequestId.IsEmpty())
    {
        return;
    }

    FScopeLock Lock(&ResponsePolicyMutex);
    // A new accepted request owns the ID. Replacing instead of OR-ing prevents a
    // reused ID from inheriting a stale mutating policy from an abandoned request.
    FResponsePolicy& Policy = ResponsePolicies.FindOrAdd(RequestId);
    Policy.bMutating = bMutating;
    Policy.Method = Method;
}

void UPinWrightSubsystem::RegisterNestedResponsePolicy(
    const FString& RequestId, const bool bMutating, const FString& Method)
{
    if (RequestId.IsEmpty())
    {
        return;
    }

    FScopeLock Lock(&ResponsePolicyMutex);
    FResponsePolicy& ExistingPolicy = ResponsePolicies.FindOrAdd(RequestId);
    // Cross-dispatch is part of the accepted outer request, not a new transport
    // request. Preserve a mutating outer policy across the nested handler.
    ExistingPolicy.bMutating = ExistingPolicy.bMutating || bMutating;
    if (ExistingPolicy.Method.IsEmpty())
    {
        ExistingPolicy.Method = Method;
    }
}

void UPinWrightSubsystem::DecorateAutomationResponse(
    const FString& RequestId, const bool bSuccess,
    TSharedPtr<FJsonObject>& Result)
{
    FResponsePolicy Policy;
    {
        FScopeLock Lock(&ResponsePolicyMutex);
        if (const FResponsePolicy* FoundPolicy = ResponsePolicies.Find(RequestId))
        {
            Policy = *FoundPolicy;
            ResponsePolicies.Remove(RequestId);
        }
    }

    if (Policy.bMutating)
    {
        if (!Result.IsValid())
        {
            Result = MakeShared<FJsonObject>();
        }
        PinWrightWorldPrecondition::AddWorldField(Result, Policy.Method);
    }
}

void UPinWrightSubsystem::SendAutomationResponse(
    const FString& RequestId, const bool bSuccess, const FString& Message,
    const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
{
    TSharedPtr<FJsonObject> ResponseResult = Result;
    DecorateAutomationResponse(RequestId, bSuccess, ResponseResult);

    if (StreamingTransport.IsValid())
    {
        StreamingTransport->ResolveCompletion(
            RequestId, bSuccess, Message, ResponseResult, ErrorCode);
    }
}

#if WITH_DEV_AUTOMATION_TESTS
void UPinWrightSubsystem::RegisterResponsePolicyForTesting(
    const FString& RequestId, const bool bMutating)
{
    RegisterResponsePolicy(RequestId, bMutating);
}

void UPinWrightSubsystem::DecorateResponseForTesting(
    const FString& RequestId, const bool bSuccess,
    TSharedPtr<FJsonObject>& Result)
{
    DecorateAutomationResponse(RequestId, bSuccess, Result);
}
#endif

void UPinWrightSubsystem::SendAutomationError(
    const FString& RequestId, const FString& Message, const FString& ErrorCode)
{
    const FString ResolvedError =
        ErrorCode.IsEmpty() ? TEXT("AUTOMATION_ERROR") : ErrorCode;
    UE_LOG(LogPinWrightSubsystem, Warning,
           TEXT("Automation request failed (%s): %s"), *ResolvedError,
           *SanitizeForLog(Message));
    SendAutomationResponse(RequestId, false, Message, nullptr, ResolvedError);
}

bool UPinWrightSubsystem::IsStreamingRequest(const FString& RequestId) const
{
    return bStreamingActive && StreamingTransport.IsValid() &&
           StreamingTransport->IsStreamingRequest(RequestId);
}

bool UPinWrightSubsystem::RegisterStreamingJob(
    const FString& TicketId, const FString& RequestId)
{
    if (!IsStreamingRequest(RequestId))
    {
        return false;
    }
    StreamingJobRequests.Add(TicketId, RequestId);
    return true;
}

void UPinWrightSubsystem::HandleJobEvent(
    const FString& TicketId, const FString& Event,
    const TSharedPtr<FJsonObject>& Payload,
    const TSharedPtr<FJsonObject>& ResultOrNull)
{
    const FString* RequestIdPtr = StreamingJobRequests.Find(TicketId);
    if (!RequestIdPtr || !StreamingTransport.IsValid())
    {
        return;
    }
    const FString RequestId = *RequestIdPtr;

    // Event text: prefer the payload's message, fall back to the event name.
    FString Message;
    if (Payload.IsValid())
    {
        Payload->TryGetStringField(TEXT("message"), Message);
    }
    if (Message.IsEmpty())
    {
        Message = Event;
    }

    const bool bTerminal = Event == TEXT("completed") ||
                           Event == TEXT("failed") ||
                           Event == TEXT("cancelled");
    if (bTerminal)
    {
        // Resolving the completion closes the stream; drop the mapping first so a
        // reentrant event for the same ticket can't double-resolve.
        StreamingJobRequests.Remove(TicketId);
        const bool bSuccess = Event == TEXT("completed");
        SendAutomationResponse(
            RequestId, bSuccess, Message, ResultOrNull,
            bSuccess ? FString()
                     : (Event == TEXT("cancelled") ? TEXT("JOB_CANCELLED")
                                                   : TEXT("JOB_FAILED")));
        return;
    }

    // Non-terminal (started/progress): emit a notifications/progress frame.
    // ticket_id is ALWAYS included so a dropped stream degrades to ticket polling.
    const TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();

    // Echo the CLIENT's token. MCP admits only tokens "provided in an active request"
    // (spec 2025-06-18, Progress), so the ticket id this used to send was a token the
    // client had never issued: a conforming receiver cannot match it to the call it is
    // waiting on. The ticket id still travels, in its own field, so a dropped stream
    // still degrades to polling system.job_status.
    TSharedPtr<FJsonValue> ClientToken;
    if (StreamingTransport->GetProgressToken(RequestId, ClientToken) && ClientToken.IsValid())
    {
        ParamsObj->SetField(TEXT("progressToken"), ClientToken);
    }
    else
    {
        // Unreachable in practice — the stream only exists because a token arrived —
        // but a frame with no token at all is worse than one carrying the ticket.
        ParamsObj->SetStringField(TEXT("progressToken"), TicketId);
    }

    // FJobRegistry::RecordProgress is the single writer of the progress number and
    // guarantees it increases across frames; "started" carries none and is frame zero.
    double ProgressValue = 0.0;
    if (Payload.IsValid())
    {
        Payload->TryGetNumberField(TEXT("progress"), ProgressValue);
    }
    ParamsObj->SetNumberField(TEXT("progress"), ProgressValue);
    double TotalValue = 0.0;
    if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("total"), TotalValue))
    {
        // Optional per spec, and omitted rather than zero-filled when the verb does not
        // know its denominator — a total of 0 would render as a finished bar.
        ParamsObj->SetNumberField(TEXT("total"), TotalValue);
    }
    ParamsObj->SetStringField(TEXT("message"), Message);
    ParamsObj->SetStringField(TEXT("ticket_id"), TicketId);

    const TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
    Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    Notification->SetStringField(TEXT("method"), TEXT("notifications/progress"));
    Notification->SetObjectField(TEXT("params"), ParamsObj);

    if (!StreamingTransport->WriteStreamFrame(RequestId, Notification))
    {
        // Dead stream: stop forwarding; the caller degrades to ticket polling.
        StreamingJobRequests.Remove(TicketId);
    }
}

void UPinWrightSubsystem::RegisterHandler(
    const FString& Action, FAutomationHandler Handler)
{
    if (Dispatcher.IsValid())
    {
        Dispatcher->RegisterHandler(Action, Handler);
    }
}

void UPinWrightSubsystem::GetRegisteredToolKeys(
    TArray<FString>& OutToolKeys) const
{
    if (Dispatcher.IsValid())
    {
        Dispatcher->GetRegisteredToolKeys(OutToolKeys);
    }
}

bool UPinWrightSubsystem::DispatchMethod(
    const FString& MethodName, const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload)
{
    if (Dispatcher.IsValid())
    {
        return Dispatcher->DispatchMethod(MethodName, RequestId, Payload);
    }
    return false;
}

AActor* UPinWrightSubsystem::FindActorByName(const FString& Target)
{
    return McpActorUtils::FindActorByName(nullptr, Target);
}

bool UPinWrightSubsystem::ExecuteEditorCommands(
    const TArray<FString>& Commands, FString& OutErrorMessage)
{
    check(IsInGameThread());

    if (!GEditor)
    {
        OutErrorMessage = TEXT("Editor not available");
        return false;
    }

    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!EditorWorld)
    {
        OutErrorMessage = TEXT("Editor world context not available");
        return false;
    }

    for (const FString& Command : Commands)
    {
        if (Command.IsEmpty())
        {
            continue;
        }

        if (!GEditor->Exec(EditorWorld, *Command))
        {
            OutErrorMessage =
                FString::Printf(TEXT("Failed to execute command: %s"), *Command);
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("ExecuteEditorCommands: %s"), *OutErrorMessage);
            return false;
        }

        UE_LOG(LogPinWrightSubsystem, Verbose,
               TEXT("ExecuteEditorCommands: Executed '%s'"), *Command);
    }

    return true;
}
