// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/FormatterRegistration.h"

PINWRIGHT_API DECLARE_LOG_CATEGORY_EXTERN(LogRpcDispatcher, Log, All);

class UPinWrightSubsystem;
class FAsyncRequestLifetimeLease;
struct FResponseCapture;

// Handler function signature (old-style TMap dispatch)
using FAutomationHandler = TFunction<bool(const FString&, const FString&,
                                          const TSharedPtr<FJsonObject>&)>;

// Completion sink: where the dispatcher's direct-resolve paths (unknown action,
// text-format envelope, exception catch, missing-handler-response guard) deliver
// their responses. Decouples the dispatcher from the concrete transport type.
// Signature mirrors UPinWrightSubsystem::SendAutomationResponse.
using FResponseSink = TFunction<void(const FString& RequestId, bool bSuccess,
                                     const FString& Message,
                                     const TSharedPtr<FJsonObject>& Result,
                                     const FString& ErrorCode)>;

class PINWRIGHT_API FRpcDispatcher
{
public:
    FRpcDispatcher();
    ~FRpcDispatcher();

    // Transport-agnostic initialization: all direct-resolve responses go
    // through the sink.
    void Initialize(FResponseSink InSink);

    // Register a handler (called during initialization)
    void RegisterHandler(const FString& MethodName, FAutomationHandler Handler);

    // Drain auto-registered handlers from the static queue
    void DrainAutoRegistrations(UPinWrightSubsystem* Subsystem);

    // Drain auto-registered text formatters from the static queue (call after DrainAutoRegistrations)
    void DrainFormatterRegistrations();

    // Access registered formatters (for tests / introspection)
    const TMap<FString, FRpcFormatterFunc>& GetFormatters() const { return Formatters; }

    // Process a request (game thread, with reentrancy guard)
    void ProcessRequest(const FString& RequestId, const FString& Method,
                        const TSharedPtr<FJsonObject>& Params);

    // Process deferred requests (called on Tick)
    void ProcessPendingRequests();

    // Dispatch a single method by name (used by other handlers for forwarding)
    bool DispatchMethod(const FString& MethodName, const FString& RequestId,
                        const TSharedPtr<FJsonObject>& Payload);

    // Access registered handlers (for catalog and external queries)
    const TMap<FString, FAutomationHandler>& GetHandlers() const { return Handlers; }
    const TMap<FString, FHandlerRegistration>& GetAutoRegisteredHandlers() const { return AutoRegistered; }

    // Monotonic counter bumped on every registry mutation. Consumers that cache a
    // view over the registry (e.g. the wiki catalog) compare this against their
    // cached value to detect post-init registrants and rebuild when it changes.
    uint32 GetRegistryGeneration() const { return RegistryGeneration; }

    // Get registered handler keys
    void GetRegisteredToolKeys(TArray<FString>& OutToolKeys) const;

#if WITH_DEV_AUTOMATION_TESTS
    // Test seam: inject an auto-registration record directly so cache-invalidation
    // tests can simulate a post-init registrant without the static drain pipeline.
    void AddAutoRegisteredForTesting(const FHandlerRegistration& Reg)
    {
        AutoRegistered.Add(Reg.MethodName, Reg);
        ++RegistryGeneration;
    }

    bool IsProcessingRequestForTesting() const { return bProcessingRequest; }

    // Test seam for direct/nested precondition assertions. DispatchMethod normally
    // routes its context errors through the owning subsystem; a standalone test
    // dispatcher can instead capture the response without initializing transport.
    void SetResponseCaptureForTesting(FResponseCapture* InCapture)
    {
        ResponseCaptureForTesting = InCapture;
    }
#endif

private:
    friend class FHandlerContext;

    // Publish mutation metadata before this request can be queued or handed to
    // a handler, so late fanout/async responses still reach the shared decorator.
    void RegisterResponsePolicyForRequest(const FString& RequestId,
                                           const FString& Method);

    struct FSafePointContinuationLifetime
    {
        FRpcDispatcher* Dispatcher = nullptr;
        uint64 LastAsyncRequestLifetimeId = 0;
        TMap<uint64, TWeakPtr<FAsyncRequestLifetimeLease>> AsyncRequestLifetimes;
    };

    // Run an in-handler safe-point continuation without releasing the active
    // request's reentrancy guard. If the dispatcher ends before the ticker can
    // run, invoke the optional abandonment callback instead of Work. Returns
    // false when there is no active request.
    bool DeferActiveRequestToSafePoint(
        TFunction<void()> Work, const TCHAR* Reason,
        TFunction<void()> OnOwnerAbandoned = TFunction<void()>());
    TSharedPtr<FAsyncRequestLifetimeLease> RetainAsyncRequestLifetime(
        TFunction<void()> OnOwnerAbandoned);
    void FinishActiveRequest();

    TMap<FString, FAutomationHandler> Handlers;
    TMap<FString, FHandlerRegistration> AutoRegistered;
    uint32 RegistryGeneration = 0;
    TMap<FString, FRpcFormatterFunc> Formatters;
    FResponseSink ResponseSink;
    UPinWrightSubsystem* SubsystemPtr = nullptr;

    // Reentrancy guard
    bool bProcessingRequest = false;
    int32 PendingSafePointContinuations = 0;
    TSharedRef<FSafePointContinuationLifetime> SafePointContinuationLifetime;

#if WITH_DEV_AUTOMATION_TESTS
    FResponseCapture* ResponseCaptureForTesting = nullptr;
#endif

    // Deferred request queue
    struct FDeferredRequest
    {
        FString RequestId;
        FString Method;
        TSharedPtr<FJsonObject> Params;
    };
    TArray<FDeferredRequest> PendingQueue;
    FCriticalSection PendingQueueMutex;
    bool bPendingScheduled = false;
};
