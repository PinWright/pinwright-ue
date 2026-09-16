// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EditorSubsystem.h"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"
#include "Transport/BindRetryPolicy.h"
#include "PinWrightSubsystem.generated.h"

class FSocketHttpServer;
class FRpcDispatcher;
class FToolCatalog;

UENUM(BlueprintType)
enum class EPinWrightState : uint8 {
    Disconnected,
    Connected
};

// Resolved state of the HTTP MCP listener, for status display.
UENUM(BlueprintType)
enum class EMcpServerStatus : uint8 {
    Disabled,    // transport off in settings, or never started
    Listening,   // bound and serving on the configured port
    PortInUse    // the configured port is held by another process; this editor is not serving
};

PINWRIGHT_API DECLARE_LOG_CATEGORY_EXTERN(LogPinWrightSubsystem, Log, All);

UCLASS()
class PINWRIGHT_API UPinWrightSubsystem
    : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // Registered by the module at startup, before editor subsystems are created,
    // so the one-shot editor-initialized boundary cannot be missed by a late or
    // recreated subsystem instance.
    static void RecordEditorInitialized(double StartupDurationSeconds);

    UFUNCTION(BlueprintCallable, Category = "MCP Automation")
    bool IsBridgeActive() const;

    UFUNCTION(BlueprintCallable, Category = "MCP Automation")
    EPinWrightState GetBridgeState() const;

    // Actually-bound HTTP transport port; 0 when the transport is inactive.
    int32 GetBoundHttpPort() const;

    // Resolved listener status for the setup screen's status banner.
    EMcpServerStatus GetServerStatus() const;

    // ---- Bind-failure introspection. A bind-failed editor serves no RPC at all, so nothing
    // it could answer would report this; these exist so the in-editor status surfaces and the
    // log say WHICH port lost and whether the retry schedule is still live, instead of leaving
    // "process running but never reachable" to be inferred from tasklist.

    // The port this editor tried and failed to bind, or 0. Non-zero only while
    // GetServerStatus() is PortInUse.
    int32 GetContestedHttpPort() const { return ContestedPort; }

    // Failed bind attempts so far, counting the one at startup as the first.
    int32 GetBindAttemptCount() const;

    // True once the bounded retry budget is spent: this editor will never serve MCP without
    // a settings change and a restart.
    bool IsBindRetryExhausted() const;

    // True only after the editor-initialized boundary and the cold-start safety
    // conditions required by public PinWright operations are satisfied.
    bool IsEditorReady() const { return bEditorReady; }

    // Response helpers (delegate to transport)
    void SendAutomationResponse(const FString& RequestId, bool bSuccess,
                                const FString& Message,
                                const TSharedPtr<FJsonObject>& Result = nullptr,
                                const FString& ErrorCode = FString());
    void SendAutomationError(const FString& RequestId, const FString& Message,
                             const FString& ErrorCode);

    bool ExecuteEditorCommands(const TArray<FString>& Commands,
                               FString& OutErrorMessage);

    // Handler type alias (kept for external consumers)
    using FAutomationHandler = TFunction<bool(const FString&, const FString&,
                                              const TSharedPtr<FJsonObject>&)>;

    // Registration (delegates to dispatcher)
    void RegisterHandler(const FString& Action, FAutomationHandler Handler);
    void GetRegisteredToolKeys(TArray<FString>& OutToolKeys) const;

    // Dispatch a request to a registered handler by method name
    bool DispatchMethod(const FString& MethodName, const FString& RequestId,
                        const TSharedPtr<FJsonObject>& Payload);

    // Actor lookup wrapper (delegates to McpActorUtils::FindActorByName)
    AActor* FindActorByName(const FString& Target);

    // True when the streaming transport is active and the request holds an open
    // SSE stream (job handlers use this to pick streaming over the ticket reply).
    bool IsStreamingRequest(const FString& RequestId) const;

    // Map a job ticket onto an open SSE stream so OnJobEvent frames flow to it.
    // Returns true only when the streaming transport is active and RequestId is a
    // live stream; false means the caller should fall back to the ticket response.
    bool RegisterStreamingJob(const FString& TicketId, const FString& RequestId);

    // Dispatcher accessor for catalog-side callers (wiki handler reads the registry).
    TSharedPtr<FRpcDispatcher> GetDispatcher() const { return Dispatcher; }

#if WITH_DEV_AUTOMATION_TESTS
    // Test seam for the shared response-policy decorator. It does not initialize the
    // transport, so policy tests can cover fanout/error/read-only behavior without PIE
    // or host assets.
    void RegisterResponsePolicyForTesting(const FString& RequestId, bool bMutating);
    void DecorateResponseForTesting(const FString& RequestId, bool bSuccess,
                                    TSharedPtr<FJsonObject>& Result);
#endif

private:
    bool Tick(float DeltaTime);

    // Register before a request is queued or entered so a later async/fanout response
    // can be decorated even when it bypasses FHandlerContext.
    void RegisterResponsePolicy(const FString& RequestId, bool bMutating,
                                const FString& Method = FString());

    // A cross-dispatch reuses the accepted request's ID; preserve the outer mutation
    // policy while the nested handler runs under that same response boundary.
    void RegisterNestedResponsePolicy(const FString& RequestId, bool bMutating,
                                      const FString& Method = FString());

    // The single transport-facing response decorator. Mutating success and error
    // envelopes both receive the resolved target-world echo.
    void DecorateAutomationResponse(const FString& RequestId, bool bSuccess,
                                    TSharedPtr<FJsonObject>& Result);

    void RefreshEditorReadiness();

    // Bind the transport and, on success, do the whole activation - bookkeeping, the gateway
    // port file the stdio proxy reads, and the job-event bridge. One helper rather than two
    // copies, because a late bind that skipped any of it would leave a transport that answers
    // HTTP while the proxy still points at the old port and streamed jobs emit nothing.
    bool TryStartTransport(int32 Port, bool bIsRetry);

    // Drives the bounded bind-retry schedule from the existing 0.1s ticker, and retries a
    // publication that failed after a bind that succeeded.
    void TickBindRetry();

    // Publish the bound port for the stdio proxy. Failure is NOT swallowed: a bound server
    // whose port never reached the file is unreachable by every caller, so it is logged at
    // Error and re-attempted from the ticker until it lands.
    void PublishBoundPort(int32 Port);

    // Job-registry event bridge: forwards progress frames for streamed jobs to
    // their open SSE stream and resolves the stream on terminal events.
    void HandleJobEvent(const FString& TicketId, const FString& Event,
                        const TSharedPtr<FJsonObject>& Payload,
                        const TSharedPtr<FJsonObject>& ResultOrNull);

    // Decomposed components. The socket-based SSE-capable server is the single
    // transport.
    TSharedPtr<FSocketHttpServer> StreamingTransport;
    TSharedPtr<FRpcDispatcher> Dispatcher;
    TSharedPtr<FToolCatalog> Catalog;

    // Streaming transport bookkeeping (FSocketHttpServer exposes no port/active
    // accessors, so the subsystem tracks the Start() outcome itself).
    bool bStreamingActive = false;
    int32 StreamingPort = 0;

    // Bind-retry state. The port the startup bind lost, and the bounded backoff schedule that
    // keeps trying it. ContestedPort stays set after the budget is spent - it is the answer to
    // "why is this editor not serving", which outlives the retrying.
    int32 ContestedPort = 0;
    PinWrightBindRetry::FState BindRetry;

    // Set when a bind succeeded but the gateway-port file could not be written. Serving on a
    // port no proxy can discover is a silent outage of exactly the kind this whole path
    // exists to end, so the publication is retried rather than left to the next launch.
    bool bPortPublishPending = false;

    // Startup readiness is latched at FEditorDelegates::OnEditorInitialized and
    // remains false until package loading is idle and the editor element
    // selection set exists. This is read on the game thread by the dispatcher and
    // published as atomics to the socket transport's I/O thread.
    bool bEditorInitialized = false;
    bool bEditorReady = false;

    // Streamed jobs: ticket id -> request id of the SSE stream carrying its events.
    TMap<FString, FString> StreamingJobRequests;

    // Handle for the FJobRegistry::OnJobEvent subscription.
    FDelegateHandle JobEventHandle;

    // Ticker handle
    FTSTicker::FDelegateHandle TickHandle;

    // Request IDs remain registered across handler return and async work until the
    // response boundary consumes them. A newly accepted ID replaces any prior entry;
    // subsystem shutdown clears entries whose disconnected requests never responded.
    // The lock also covers async completions that resolve from a worker thread.
    mutable FCriticalSection ResponsePolicyMutex;
    struct FResponsePolicy
    {
        bool bMutating = false;
        FString Method;
    };
    TMap<FString, FResponsePolicy> ResponsePolicies;

    // FHandlerContext needs access to private members for construction
    friend class FHandlerContext;
    friend class FRpcDispatcher;
};
