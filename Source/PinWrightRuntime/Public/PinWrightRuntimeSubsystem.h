// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/EngineSubsystem.h"
#include "Templates/SharedPointer.h"
#include "Transport/BindRetryPolicy.h"
#include "PinWrightRuntimeSubsystem.generated.h"

class AActor;
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

PINWRIGHTRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogPinWrightSubsystem, Log, All);

// Host of the MCP endpoint in every process kind (docs/runtime-module.md): drains handler
// registrations, owns the catalog, the transport and its bind retry, the 0.1 s ticker and the
// job/event bridge. Editor duties plug in through IPinWrightEditorHooks.
UCLASS()
class PINWRIGHTRUNTIME_API UPinWrightRuntimeSubsystem : public UEngineSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // The live instance, or null before engine init / in a commandlet.
    static UPinWrightRuntimeSubsystem* Get();

    bool IsBridgeActive() const;
    EPinWrightState GetBridgeState() const;

    // Actually-bound HTTP transport port; 0 when the transport is inactive.
    int32 GetBoundHttpPort() const;

    // Resolved listener status for the setup screen's status banner.
    EMcpServerStatus GetServerStatus() const;

    // ---- Bind-failure introspection. A bind-failed editor serves no RPC at all, so nothing
    // it could answer would report this; these exist so the in-editor status surfaces and the
    // log say WHICH port lost and whether the retry schedule is still live.

    // The port this process tried and failed to bind, or 0. Non-zero only while
    // GetServerStatus() is PortInUse.
    int32 GetContestedHttpPort() const { return ContestedPort; }

    // Failed bind attempts so far, counting the one at startup as the first.
    int32 GetBindAttemptCount() const;

    // True once the bounded retry budget is spent.
    bool IsBindRetryExhausted() const;

    // True only after the startup boundary and the cold-start safety conditions required by
    // public PinWright operations are satisfied (editor: hooks; game: engine initialized).
    bool IsEditorReady() const { return bEditorReady; }

    // True for the editor that owns gateway-port / gateway-token (no -PinWrightPort).
    bool IsPrimaryInstance() const { return bPrimaryInstance; }

    // Response helpers (delegate to transport)
    void SendAutomationResponse(const FString& RequestId, bool bSuccess,
                                const FString& Message,
                                const TSharedPtr<FJsonObject>& Result = nullptr,
                                const FString& ErrorCode = FString());
    void SendAutomationError(const FString& RequestId, const FString& Message,
                             const FString& ErrorCode);

    // Console commands against the editor world (editor) or the game world (game).
    bool ExecuteEditorCommands(const TArray<FString>& Commands, FString& OutErrorMessage);

    using FAutomationHandler = TFunction<bool(const FString&, const FString&,
                                              const TSharedPtr<FJsonObject>&)>;

    void RegisterHandler(const FString& Action, FAutomationHandler Handler);
    void GetRegisteredToolKeys(TArray<FString>& OutToolKeys) const;

    bool DispatchMethod(const FString& MethodName, const FString& RequestId,
                        const TSharedPtr<FJsonObject>& Payload);

    // Actor lookup wrapper (delegates to McpActorUtils::FindActorByName)
    AActor* FindActorByName(const FString& Target);

    // True when the streaming transport is active and the request holds an open SSE stream.
    bool IsStreamingRequest(const FString& RequestId) const;

    // Map a job ticket onto an open SSE stream so OnJobEvent frames flow to it. False means
    // the caller should fall back to the ticket response.
    bool RegisterStreamingJob(const FString& TicketId, const FString& RequestId);

    TSharedPtr<FRpcDispatcher> GetDispatcher() const { return Dispatcher; }

#if WITH_DEV_AUTOMATION_TESTS
    void RegisterResponsePolicyForTesting(const FString& RequestId, bool bMutating);
    void DecorateResponseForTesting(const FString& RequestId, bool bSuccess,
                                    TSharedPtr<FJsonObject>& Result);

    // Startup bind on an uninitialized (NewObject) subsystem, as the primary editor. Only for
    // ports expected to FAIL: a successful bind would publish gateway-port (override its root first).
    bool StartTransportForTesting(int32 Port);

    // The real activation path (gate -> bind -> gateway-port for the primary editor -> instance
    // file) on an uninitialized subsystem, with CommandLine read for every switch and
    // bGameProcess deciding the process kind. True when serving.
    bool ActivateForTesting(const TCHAR* CommandLine, bool bGameProcess);
    // Stop the transport and delete the instance file (the clean-shutdown path).
    void DeactivateForTesting();
#endif

private:
    bool Tick(float DeltaTime);

    // Decide from CommandLine whether and where to serve, then bind. Returns true when serving.
    bool Activate(const TCHAR* CommandLine, bool bGameProcess, bool bIsolatedTestChild);
    void StopServing();

    void RegisterResponsePolicy(const FString& RequestId, bool bMutating,
                                const FString& Method = FString());
    void RegisterNestedResponsePolicy(const FString& RequestId, bool bMutating,
                                      const FString& Method = FString());
    void DecorateAutomationResponse(const FString& RequestId, bool bSuccess,
                                    TSharedPtr<FJsonObject>& Result);

    void RefreshEditorReadiness();

    // Bind and, on success, do the whole activation: bookkeeping, the gateway-port file
    // (primary only), the instance file and the job-event bridge.
    bool TryStartTransport(int32 Port, bool bIsRetry);
    void TickBindRetry();
    void PublishBoundPort(int32 Port);

    void HandleJobEvent(const FString& TicketId, const FString& Event,
                        const TSharedPtr<FJsonObject>& Payload,
                        const TSharedPtr<FJsonObject>& ResultOrNull);

    TSharedPtr<FSocketHttpServer> StreamingTransport;
    TSharedPtr<FRpcDispatcher> Dispatcher;
    TSharedPtr<FToolCatalog> Catalog;

    bool bStreamingActive = false;
    int32 StreamingPort = 0;

    int32 ContestedPort = 0;
    PinWrightBindRetry::FState BindRetry;
    bool bPortPublishPending = false;

    // Instance identity: primary = editor without -PinWrightPort (owns gateway-port, retries
    // its bind); others bind their own port once. Token and launch reason go to the instance file.
    bool bPrimaryInstance = true;
    bool bGameProcess = false;
    FString InstanceToken;
    FString LaunchReason;
    // The instance file this subsystem wrote, deleted on clean shutdown.
    FString InstanceFilePath;

    bool bEditorReady = false;

    TMap<FString, FString> StreamingJobRequests;
    FDelegateHandle JobEventHandle;
    FTSTicker::FDelegateHandle TickHandle;

    mutable FCriticalSection ResponsePolicyMutex;
    struct FResponsePolicy
    {
        bool bMutating = false;
        FString Method;
    };
    TMap<FString, FResponsePolicy> ResponsePolicies;

    friend class FHandlerContext;
    friend class FRpcDispatcher;
};
