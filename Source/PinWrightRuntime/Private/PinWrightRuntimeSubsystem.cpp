// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightRuntimeSubsystem.h"

#include "Catalog/ToolCatalog.h"
#include "Dispatch/PinWrightWorld.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/WorldPrecondition.h"
#include "PinWrightEditorHooks.h"
#include "PinWrightSettings.h"
#include "State/PluginState.h"
#include "Transport/ModalStateProbe.h"
#include "Transport/PortAdvertisement.h"
#include "Transport/SocketHttpServer.h"
#include "Transport/TransportLaunchPolicy.h"
#include "Utils/ActorUtils.h"
#include "Utils/GatewayAuthToken.h"
#include "Utils/GatewayPortFile.h"
#include "Utils/HttpResponseSpill.h"
#include "Utils/InstanceFile.h"
#include "Utils/JobMonitorLog.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogPinWrightSubsystem);

namespace PinWrightRuntimeSubsystemDetail
{
    // Sanitize incoming text for logging: replace control characters
    FString SanitizeForLog(const FString& In)
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

    FString CurrentMapName()
    {
        const UWorld* World = PinWright::GetTargetWorld();
        return World && World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
    }
}

UPinWrightRuntimeSubsystem* UPinWrightRuntimeSubsystem::Get()
{
    return GEngine ? GEngine->GetEngineSubsystem<UPinWrightRuntimeSubsystem>() : nullptr;
}

void UPinWrightRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
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

    IPinWrightEditorHooks* Hooks = PinWright::GetEditorHooks();

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

    // The dispatcher resolves ALL responses (handler results and its own direct-resolve
    // paths: unknown-action, _format:text, exception catches) through this sink.
    Dispatcher->Initialize([this](const FString& RequestId, bool bSuccess, const FString& Message,
                                  const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
    {
        SendAutomationResponse(RequestId, bSuccess, Message, Result, ErrorCode);
    });

    // Load engine-plugin integration sub-modules (editor). Must precede DrainAutoRegistrations
    // so their static-init registrations drain with the rest, land in the startup wiki
    // generation, and precede transport start.
    if (Hooks)
    {
        Hooks->BeforeDrainRegistrations();
    }

    // Drain auto-registered handlers (must happen before Catalog caches namespace counts)
    Dispatcher->DrainAutoRegistrations(this);

    // Drain auto-registered text formatters, wired before any request can arrive.
    Dispatcher->DrainFormatterRegistrations();

    Catalog->Initialize(Dispatcher);

    // Isolated test children share the project directory with the host editor.
    // Keep their startup read-only outside their private automation log directory.
    if (Hooks && !bIsolatedTestChild)
    {
        Hooks->AfterCatalogInit();
    }

    const TWeakPtr<FRpcDispatcher> WeakDispatcher = Dispatcher;
    StreamingTransport->OnRequestReceived.BindLambda(
        [WeakDispatcher](const FString& RequestId, const FString& Method,
                         const TSharedPtr<FJsonObject>& Params)
        {
            if (const TSharedPtr<FRpcDispatcher> PinnedDispatcher = WeakDispatcher.Pin())
            {
                PinnedDispatcher->ProcessRequest(RequestId, Method, Params);
            }
        });

    Activate(FCommandLine::Get(), PinWright::IsGameProcess(), bIsolatedTestChild);

    TickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UPinWrightRuntimeSubsystem::Tick), 0.1f);

    UE_LOG(LogPinWrightSubsystem, Log,
           TEXT("PinWrightSubsystem initialized (socket transport)."));
}

bool UPinWrightRuntimeSubsystem::Activate(
    const TCHAR* CommandLine, const bool bInGameProcess, const bool bIsolatedTestChild)
{
#if UE_BUILD_SHIPPING
    // The module is denied for Shipping in the .uplugin; this keeps a mis-packaged build closed.
    return false;
#endif
    bGameProcess = bInGameProcess;
    LaunchReason.Reset();
    FParse::Value(CommandLine, TEXT("PinWrightLaunchReason="), LaunchReason);

    FString PortText;
    const bool bExplicitPort = FParse::Value(CommandLine, TEXT("PinWrightPort="), PortText);
    const bool bBareSwitch = FParse::Param(CommandLine, TEXT("PinWright"));
    bPrimaryInstance = !bInGameProcess && !bExplicitPort;

    if (bIsolatedTestChild)
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWright HTTP transport disabled for isolated automation child."));
        return false;
    }

    if (bInGameProcess && !bExplicitPort && !bBareSwitch)
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWright is not serving in this game process; launch with -PinWright or "
                    "-PinWrightPort=<N|0> to drive it."));
        return false;
    }

    const FString TransportSuppressedBy =
        PinWrightTransportLaunchPolicy::SuppressionReason(CommandLine);
    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();

    if (!StreamingTransport.IsValid())
    {
        StreamingTransport = MakeShared<FSocketHttpServer>();
    }

    if (bPrimaryInstance)
    {
        // Wipe jobs.jsonl and all rotation segments from the previous session. A launch that does
        // not bind the transport does not own them: the checkout's MCP editor may be writing them.
        // Only the primary editor owns them; a -PinWrightPort or game instance never wipes them.
        if (TransportSuppressedBy.IsEmpty())
        {
            const FString JobsPath = FPaths::ProjectDir() / JobMonitorLog::JobsJsonlRelativePath;
            IFileManager& FM = IFileManager::Get();
            FM.Delete(*JobsPath, /*RequireExists=*/false, /*EvenReadOnly=*/false);
            for (int32 i = 1; i <= Settings->MonitorFileRotationKeep; ++i)
            {
                const FString RotPath = FString::Printf(TEXT("%s.%d"), *JobsPath, i);
                FM.Delete(*RotPath, /*RequireExists=*/false, /*EvenReadOnly=*/false);
            }
        }

        if (!TransportSuppressedBy.IsEmpty())
        {
            UE_LOG(LogPinWrightSubsystem, Log,
                   TEXT("PinWright HTTP transport not started (%s); the checkout's MCP port stays "
                        "free. Pass -PinWrightTransport to bind it."),
                   *TransportSuppressedBy);
            return false;
        }
        if (!Settings || !Settings->bEnableHttpTransport)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("HTTP transport is disabled in settings."));
            return false;
        }

        HttpResponseSpill::PruneOldHttpResponseSpills();
        const int32 EffectivePort = UPinWrightSettings::ResolveHttpPort(Settings);
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("HTTP transport binding port %d."), EffectivePort);
        // One token answers both ways: the instance file carries the gateway-token.
        InstanceToken = GatewayAuthToken::GetOrCreateToken();
        return TryStartTransport(EffectivePort, /*bIsRetry=*/false);
    }

    // A -PinWright / -PinWrightPort instance: its own port, its own token, never gateway-port.
    // An explicit switch is a request to serve, so the automation-run suppression does not apply;
    // only -PinWrightNoTransport still opts out.
    if (FParse::Param(CommandLine, TEXT("PinWrightNoTransport")))
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWright HTTP transport not started (-PinWrightNoTransport)."));
        return false;
    }

    int32 Port = 0; // bare -PinWright in a game: ephemeral, discovered through the instance file
    if (bExplicitPort)
    {
        PortText.TrimStartAndEndInline();
        if (!PortText.IsNumeric() || FCString::Atoi(*PortText) < 0 || FCString::Atoi(*PortText) > 65535)
        {
            UE_LOG(LogPinWrightSubsystem, Error,
                   TEXT("-PinWrightPort=%s is not a port (0-65535; 0 picks a free one). Not serving."),
                   *PortText);
            return false;
        }
        Port = FCString::Atoi(*PortText);
    }

    InstanceToken = GatewayAuthToken::GenerateRandomToken();
    if (InstanceToken.IsEmpty())
    {
        UE_LOG(LogPinWrightSubsystem, Error,
               TEXT("No CSPRNG token for this instance; not serving without authentication."));
        return false;
    }
    StreamingTransport->SetInstanceAuthToken(InstanceToken);

    UE_LOG(LogPinWrightSubsystem, Log,
           TEXT("HTTP transport binding port %d (%s instance; %s)."), Port,
           bInGameProcess ? TEXT("game") : TEXT("editor"),
           Port == 0 ? TEXT("ephemeral") : TEXT("-PinWrightPort"));
    return TryStartTransport(Port, /*bIsRetry=*/false);
}

void UPinWrightRuntimeSubsystem::StopServing()
{
    if (JobEventHandle.IsValid())
    {
        FPluginState::Get().GetJobRegistry().OnJobEvent().Remove(JobEventHandle);
        JobEventHandle.Reset();
    }
    StreamingJobRequests.Empty();

    if (StreamingTransport.IsValid())
    {
        StreamingTransport->FailAllCompletions(TEXT("Automation bridge shutting down."),
                                               TEXT("BRIDGE_SHUTTING_DOWN"));
        StreamingTransport->Stop();
    }
    StreamingTransport.Reset();

    PinWrightInstanceFile::Delete(InstanceFilePath);
    InstanceFilePath.Reset();

    bStreamingActive = false;
    StreamingPort = 0;
    ContestedPort = 0;
    BindRetry.Reset();
    bPortPublishPending = false;
}

void UPinWrightRuntimeSubsystem::Deinitialize()
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

    StopServing();

    // FailAllCompletions gives normal pending requests their terminal error response.
    // DropCompletionsForConnection intentionally has no callback for a disconnected client,
    // so discard any remaining per-session policies at the subsystem boundary.
    {
        FScopeLock Lock(&ResponsePolicyMutex);
        ResponsePolicies.Empty();
    }

    Super::Deinitialize();
}

bool UPinWrightRuntimeSubsystem::IsBridgeActive() const
{
    return StreamingTransport.IsValid() && bStreamingActive;
}

EPinWrightState UPinWrightRuntimeSubsystem::GetBridgeState() const
{
    return IsBridgeActive() ? EPinWrightState::Connected : EPinWrightState::Disconnected;
}

int32 UPinWrightRuntimeSubsystem::GetBoundHttpPort() const
{
    return (StreamingTransport.IsValid() && bStreamingActive) ? StreamingPort : 0;
}

EMcpServerStatus UPinWrightRuntimeSubsystem::GetServerStatus() const
{
    if (!StreamingTransport.IsValid())
    {
        return EMcpServerStatus::Disabled;
    }
    if (bStreamingActive)
    {
        return EMcpServerStatus::Listening;
    }
    // A lost bind is PortInUse, not Disabled: the setup screen's port-conflict banner and the
    // module's show-setup-screen-on-problem path exist for exactly this case.
    return (ContestedPort != 0) ? EMcpServerStatus::PortInUse : EMcpServerStatus::Disabled;
}

int32 UPinWrightRuntimeSubsystem::GetBindAttemptCount() const
{
    return BindRetry.GetFailedAttempts();
}

bool UPinWrightRuntimeSubsystem::IsBindRetryExhausted() const
{
    return BindRetry.IsExhausted();
}

bool UPinWrightRuntimeSubsystem::TryStartTransport(int32 Port, bool bIsRetry)
{
    if (!StreamingTransport.IsValid())
    {
        return false;
    }

    FSocketHttpServer::EStartResult StartResult = FSocketHttpServer::EStartResult::Unavailable;
    bStreamingActive = StreamingTransport->Start(static_cast<uint32>(Port), &StartResult);

    if (bStreamingActive)
    {
        StreamingPort = static_cast<int32>(StreamingTransport->GetBoundPort());
        ContestedPort = 0;
        BindRetry.NoteSucceeded();

        // The stdio proxy re-reads gateway-port before every call; only the primary editor
        // owns that file. Written HERE rather than only at startup, because a late bind that
        // skipped it would serve HTTP on a port no proxy config points at.
        if (bPrimaryInstance)
        {
            PublishBoundPort(StreamingPort);
        }

        // Every serving process advertises itself; after a rebind too, with the port it holds.
        InstanceFilePath = PinWrightInstanceFile::GetPath();
        if (PinWrightInstanceFile::Write(StreamingPort, InstanceToken,
                                         bGameProcess ? TEXT("game") : TEXT("editor"), LaunchReason,
                                         PinWrightRuntimeSubsystemDetail::CurrentMapName()))
        {
            UE_LOG(LogPinWrightSubsystem, Display,
                   TEXT("PinWright serving on 127.0.0.1:%d; instance file %s"),
                   StreamingPort, *InstanceFilePath);
        }

        // Bridge job-registry events onto open SSE streams. Guarded: a retry must not
        // subscribe a second time and double every job event.
        if (!JobEventHandle.IsValid())
        {
            JobEventHandle = FPluginState::Get().GetJobRegistry().OnJobEvent()
                .AddUObject(this, &UPinWrightRuntimeSubsystem::HandleJobEvent);
        }

        if (bIsRetry)
        {
            UE_LOG(LogPinWrightSubsystem, Log,
                   TEXT("Transport bound port %d on retry %d - MCP is serving again."),
                   Port, BindRetry.GetFailedAttempts());
        }
        return true;
    }

    if (!bPrimaryInstance)
    {
        // One bind, no retry schedule and no gateway-port reconciliation: that file is the
        // primary editor's, and whoever launched this instance chose the port.
        ContestedPort = StartResult == FSocketHttpServer::EStartResult::PortInUse ? Port : 0;
        UE_LOG(LogPinWrightSubsystem, Error,
               TEXT("Transport failed to bind port %d; this %s instance is NOT serving MCP. "
                    "Pass -PinWrightPort=0 to let the OS pick a free port."),
               Port, bGameProcess ? TEXT("game") : TEXT("editor"));
        return false;
    }

    // Not bound. Only a port collision can clear itself by waiting; a missing socket
    // subsystem or a failed I/O thread cannot, so those are terminal immediately.
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

            // The advertisement is wrong the moment we know we are not serving. Retracted only
            // when nothing is listening there - see Transport/PortAdvertisement.h.
            PortAdvertisement::ReconcileWhileNotServing();
        }
    }
    else
    {
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

void UPinWrightRuntimeSubsystem::PublishBoundPort(int32 Port)
{
    if (GatewayPortFile::WritePortFile(Port))
    {
        bPortPublishPending = false;
        return;
    }

    // Bound but undiscoverable: say so once per transition, then keep trying from the ticker.
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

void UPinWrightRuntimeSubsystem::TickBindRetry()
{
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
            break;
        }
        BindRetry.NoteAttemptFailed(Now);
        if (!BindRetry.IsExhausted())
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("Port %d still held after %d attempts (%.0fs); next retry in %.0fs."),
                   FailedPort, BindRetry.GetFailedAttempts(),
                   BindRetry.GetElapsedSeconds(Now),
                   BindRetry.GetNextAttemptSeconds() - Now);
        }
        break;
    }
    case PinWrightBindRetry::EAction::Relog:
        UE_LOG(LogPinWrightSubsystem, Error,
               TEXT("PinWright MCP is NOT serving: port %d was held by another process for the "
                    "whole %.0fs retry budget (%d attempts). This editor process is alive but "
                    "unreachable and will stay that way. Fix: enable \"Auto-derive Port From "
                    "Project Path\" in Project Settings (PinWright), or set a different fixed "
                    "HttpPort, then restart this editor and re-run onboarding."),
               ContestedPort, BindRetry.GetPolicy().TotalBudgetSeconds,
               BindRetry.GetFailedAttempts());
        PortAdvertisement::ReconcileWhileNotServing();
        break;

    case PinWrightBindRetry::EAction::None:
    default:
        break;
    }
}

bool UPinWrightRuntimeSubsystem::Tick(float DeltaTime)
{
    // The core ticker running at all proves no modal owns the game thread: a nested
    // Slate modal loop pumps only Slate, never FTSTicker.
    ModalStateProbe::NoteGameThreadAlive();

    RefreshEditorReadiness();

    // Before the transport tick: until the bind lands there is no transport to tick.
    TickBindRetry();

    if (StreamingTransport.IsValid())
    {
        StreamingTransport->Tick(DeltaTime);
    }

    // Process deferred requests when engine state is safe
    if (Dispatcher.IsValid() && !UE::IsSavingPackage() && !IsGarbageCollecting())
    {
        Dispatcher->ProcessPendingRequests();
    }

    // Evict completed tickets every tick (cheap; only walks completed entries past TTL).
    FPluginState::Get().GetJobRegistry().EvictExpired(FDateTime::UtcNow());

    return true;
}

void UPinWrightRuntimeSubsystem::RefreshEditorReadiness()
{
    bool bInitialized = false;
    bool bLoadingPackage = true;
    bool bSelectionSetAvailable = false;
    if (IPinWrightEditorHooks* Hooks = PinWright::GetEditorHooks())
    {
        Hooks->GetReadiness(bInitialized, bLoadingPackage, bSelectionSetAvailable);
    }
    else
    {
        // A game process has no editor startup boundary or selection set: ready once the
        // engine finished Init and a game world exists (not between maps).
        bInitialized = GEngine && GEngine->IsInitialized() && PinWright::GetTargetWorld() != nullptr;
        bLoadingPackage = false;
        bSelectionSetAvailable = true;
    }

    const bool bNextReady = bInitialized && !bLoadingPackage && bSelectionSetAvailable;
    if (bNextReady != bEditorReady)
    {
        UE_LOG(LogPinWrightSubsystem, Log,
               TEXT("PinWright editor readiness changed: %s (initialized=%s, loadingPackage=%s, selectionSet=%s)."),
               bNextReady ? TEXT("ready") : TEXT("not ready"),
               bInitialized ? TEXT("true") : TEXT("false"),
               bLoadingPackage ? TEXT("true") : TEXT("false"),
               bSelectionSetAvailable ? TEXT("true") : TEXT("false"));
    }
    bEditorReady = bNextReady;

    if (StreamingTransport.IsValid())
    {
        StreamingTransport->SetEditorReadiness(bEditorReady, bLoadingPackage, bSelectionSetAvailable);
    }
}

void UPinWrightRuntimeSubsystem::RegisterResponsePolicy(
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

void UPinWrightRuntimeSubsystem::RegisterNestedResponsePolicy(
    const FString& RequestId, const bool bMutating, const FString& Method)
{
    if (RequestId.IsEmpty())
    {
        return;
    }

    FScopeLock Lock(&ResponsePolicyMutex);
    FResponsePolicy& ExistingPolicy = ResponsePolicies.FindOrAdd(RequestId);
    // Cross-dispatch is part of the accepted outer request: preserve a mutating outer policy.
    ExistingPolicy.bMutating = ExistingPolicy.bMutating || bMutating;
    if (ExistingPolicy.Method.IsEmpty())
    {
        ExistingPolicy.Method = Method;
    }
}

void UPinWrightRuntimeSubsystem::DecorateAutomationResponse(
    const FString& RequestId, const bool bSuccess, TSharedPtr<FJsonObject>& Result)
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

void UPinWrightRuntimeSubsystem::SendAutomationResponse(
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
void UPinWrightRuntimeSubsystem::RegisterResponsePolicyForTesting(
    const FString& RequestId, const bool bMutating)
{
    RegisterResponsePolicy(RequestId, bMutating);
}

void UPinWrightRuntimeSubsystem::DecorateResponseForTesting(
    const FString& RequestId, const bool bSuccess, TSharedPtr<FJsonObject>& Result)
{
    DecorateAutomationResponse(RequestId, bSuccess, Result);
}

bool UPinWrightRuntimeSubsystem::StartTransportForTesting(const int32 Port)
{
    if (!StreamingTransport.IsValid())
    {
        StreamingTransport = MakeShared<FSocketHttpServer>();
    }
    return TryStartTransport(Port, /*bIsRetry=*/false);
}

bool UPinWrightRuntimeSubsystem::ActivateForTesting(const TCHAR* CommandLine, const bool bInGameProcess)
{
    return Activate(CommandLine, bInGameProcess, /*bIsolatedTestChild=*/false);
}

void UPinWrightRuntimeSubsystem::DeactivateForTesting()
{
    StopServing();
}
#endif

void UPinWrightRuntimeSubsystem::SendAutomationError(
    const FString& RequestId, const FString& Message, const FString& ErrorCode)
{
    const FString ResolvedError =
        ErrorCode.IsEmpty() ? TEXT("AUTOMATION_ERROR") : ErrorCode;
    UE_LOG(LogPinWrightSubsystem, Warning,
           TEXT("Automation request failed (%s): %s"), *ResolvedError,
           *PinWrightRuntimeSubsystemDetail::SanitizeForLog(Message));
    SendAutomationResponse(RequestId, false, Message, nullptr, ResolvedError);
}

bool UPinWrightRuntimeSubsystem::IsStreamingRequest(const FString& RequestId) const
{
    return bStreamingActive && StreamingTransport.IsValid() &&
           StreamingTransport->IsStreamingRequest(RequestId);
}

bool UPinWrightRuntimeSubsystem::RegisterStreamingJob(
    const FString& TicketId, const FString& RequestId)
{
    if (!IsStreamingRequest(RequestId))
    {
        return false;
    }
    StreamingJobRequests.Add(TicketId, RequestId);
    return true;
}

void UPinWrightRuntimeSubsystem::HandleJobEvent(
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

    // Echo the CLIENT's token: MCP admits only tokens "provided in an active request"
    // (spec 2025-06-18, Progress).
    TSharedPtr<FJsonValue> ClientToken;
    if (StreamingTransport->GetProgressToken(RequestId, ClientToken) && ClientToken.IsValid())
    {
        ParamsObj->SetField(TEXT("progressToken"), ClientToken);
    }
    else
    {
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
        // Omitted rather than zero-filled when the verb does not know its denominator.
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

void UPinWrightRuntimeSubsystem::RegisterHandler(const FString& Action, FAutomationHandler Handler)
{
    if (Dispatcher.IsValid())
    {
        Dispatcher->RegisterHandler(Action, Handler);
    }
}

void UPinWrightRuntimeSubsystem::GetRegisteredToolKeys(TArray<FString>& OutToolKeys) const
{
    if (Dispatcher.IsValid())
    {
        Dispatcher->GetRegisteredToolKeys(OutToolKeys);
    }
}

bool UPinWrightRuntimeSubsystem::DispatchMethod(
    const FString& MethodName, const FString& RequestId, const TSharedPtr<FJsonObject>& Payload)
{
    if (Dispatcher.IsValid())
    {
        return Dispatcher->DispatchMethod(MethodName, RequestId, Payload);
    }
    return false;
}

AActor* UPinWrightRuntimeSubsystem::FindActorByName(const FString& Target)
{
    return McpActorUtils::FindActorByName(nullptr, Target);
}

bool UPinWrightRuntimeSubsystem::ExecuteEditorCommands(
    const TArray<FString>& Commands, FString& OutErrorMessage)
{
    check(IsInGameThread());

    // GEngine is GEditor in the editor, so Exec reaches the same UEditorEngine::Exec as before.
    if (!GEngine || (!PinWright::IsGameProcess() && !PinWright::GetEditorWorldOrNull() && !GIsEditor))
    {
        OutErrorMessage = TEXT("Editor not available");
        return false;
    }

    UWorld* World = PinWright::IsGameProcess() ? PinWright::GetTargetWorld()
                                               : PinWright::GetEditorWorldOrNull();
    if (!World)
    {
        OutErrorMessage = PinWright::IsGameProcess() ? TEXT("Game world not available")
                                                     : TEXT("Editor world context not available");
        return false;
    }

    for (const FString& Command : Commands)
    {
        if (Command.IsEmpty())
        {
            continue;
        }

        if (!GEngine->Exec(World, *Command))
        {
            OutErrorMessage = FString::Printf(TEXT("Failed to execute command: %s"), *Command);
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("ExecuteEditorCommands: %s"), *OutErrorMessage);
            return false;
        }

        UE_LOG(LogPinWrightSubsystem, Verbose,
               TEXT("ExecuteEditorCommands: Executed '%s'"), *Command);
    }

    return true;
}
