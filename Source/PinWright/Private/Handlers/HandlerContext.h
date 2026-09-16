// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtr.h"

class UPinWrightSubsystem;
class FRpcDispatcher;

using FJobOnComplete = TFunction<void(bool /*bSuccess*/,
                                      TSharedPtr<FJsonObject> /*Result*/,
                                      FString /*Error*/)>;

struct FJobBindArgs
{
    FString Method;
    TSharedPtr<FJsonObject> StartedPayload;
    TFunction<void(FJobOnComplete)> BindNativeDelegate;
};

// Captures the response sent by a handler instead of forwarding to the
// transport. Used by the dispatcher when a text formatter is registered
// for the resolved method (post-processing path), and by test fixtures.
struct PINWRIGHT_API FResponseCapture
{
    bool bWasCalled = false;
    int32 CallCount = 0;
    bool bSuccess = false;
    FString ErrorCode;
    FString Message;
    TSharedPtr<FJsonObject> Result;

    void Reset()
    {
        bWasCalled = false;
        CallCount = 0;
        bSuccess = false;
        ErrorCode.Empty();
        Message.Empty();
        Result.Reset();
    }
};

#if WITH_DEV_AUTOMATION_TESTS
// Test fixture alias preserved so existing test code keeps compiling.
using FTestResponseCapture = FResponseCapture;
#endif

// Shareable late-response handle for handlers that finish their work inside an
// AsyncTask / next-tick lambda. The stack-local FHandlerContext is a value type
// that cannot safely outlive the original handler invocation, so async lambdas
// historically captured RequestId + WeakSubsystem and called the subsystem's
// response APIs directly — bypassing FResponseCapture (test fixtures, text
// formatters). This token routes through the same code path as
// FHandlerContext::SendSuccess / SendError (capture first, subsystem second),
// and can be captured by value in any number of lambdas via TSharedRef.
struct PINWRIGHT_API FAsyncResponseToken
{
    FString RequestId;
    FString Method;
    TSharedPtr<FJsonObject> Payload;
    TWeakObjectPtr<UPinWrightSubsystem> WeakSubsystem;
    // Weak (not raw) handle to the capture: an async handler's completion lambda
    // can fire long after the originating FHandlerContext — and any stack-owned
    // FResponseCapture it pointed at — has been destroyed (e.g. a test fixture's
    // local capture whose AsyncTask(GameThread) drains after RunTest returns).
    // A raw pointer here is a use-after-free; the weak handle lets SendSuccess/
    // SendError detect the freed capture and no-op instead of writing to it.
    TWeakPtr<FResponseCapture> WeakResponseCapture;

    void SendSuccess(const TSharedPtr<FJsonObject>& Result) const;
    void SendSuccess(const FString& Message, const TSharedPtr<FJsonObject>& Result) const;
    void SendError(const FString& ErrorCode, const FString& Message) const;
    void SendError(const FString& ErrorCode, const FString& Message, const TSharedPtr<FJsonObject>& Result) const;
};

// Dispatcher-owned lifetime registration for async handler work that must be
// abandoned when its originating dispatcher ends, without retaining the
// dispatcher's single-request serialization guard. The async owner captures the
// lease and calls Release once it reaches a terminal state.
class PINWRIGHT_API FAsyncRequestLifetimeLease
{
public:
    ~FAsyncRequestLifetimeLease();

    void Release();
    bool IsActive() const { return bActive; }

private:
    friend class FRpcDispatcher;

    FAsyncRequestLifetimeLease() = default;
    void Abandon();

    bool bActive = true;
    TFunction<void()> ReleaseRegistration;
    TFunction<void()> OnOwnerAbandoned;
};

class PINWRIGHT_API FHandlerContext
{
public:
    // Typed param getters (delegate to Payload JSON object)
    FString GetString(const FString& Key, const FString& Default = TEXT("")) const;
    double GetNumber(const FString& Key, double Default = 0.0) const;
    bool GetBool(const FString& Key, bool Default = false) const;
    int32 GetInt(const FString& Key, int32 Default = 0) const;
    // Strict presence-aware integer lookup. Missing returns unset; a present
    // fractional, non-finite, or out-of-int32-range value sends INVALID_PARAMS
    // and also returns unset.
    TOptional<int32> GetIntOr(const FString& Key) const;
    FVector GetVector(const FString& Key, FVector Default = FVector::ZeroVector) const;
    FRotator GetRotator(const FString& Key, FRotator Default = FRotator::ZeroRotator) const;
    TSharedPtr<FJsonObject> GetObject(const FString& Key) const;
    const TArray<TSharedPtr<FJsonValue>>* GetArray(const FString& Key) const;

    // Require + validate (sends error and returns false if missing/invalid)
    bool RequireString(const FString& Key, FString& Out) const;
    bool RequireAssetPath(const FString& Key, FString& Out) const;
    // Candidate-key overload: resolves the asset path from the first matching
    // alias in Keys (canonical first). Used by handler families whose wire
    // params drift across siblings (e.g. assetPath / materialPath / path).
    bool RequireAssetPath(const TArray<FString>& Keys, FString& Out) const;
    bool RequireInt(const FString& Key, int32& Out) const;
    bool RequireNumber(const FString& Key, double& Out) const;
    bool RequireBool(const FString& Key, bool& Out) const;
    bool RequireObject(const FString& Key, TSharedPtr<FJsonObject>& Out) const;
    bool RequireArray(const FString& Key, const TArray<TSharedPtr<FJsonValue>>*& Out) const;

    // Multi-key alias lookup (returns first non-empty match, or Default)
    FString GetStringFirstOf(const TArray<FString>& Keys, const FString& Default = TEXT("")) const;

    // Multi-key boolean lookup (returns the first key actually present, or Default).
    bool GetBoolFirstOf(const TArray<FString>& Keys, bool Default = false) const;

    // Multi-key integer lookup (presence-aware): returns the value of the first key
    // actually present, or an unset optional when none are present. Lets callers
    // distinguish "absent" from "explicitly 0" without a separate HasField guard.
    TOptional<int32> GetIntFirstOf(const TArray<FString>& Keys) const;

    // Returns the raw JSON value for the first matching key (any JSON type), or null.
    TSharedPtr<FJsonValue> GetJsonValueFirstOf(const TArray<FString>& Keys) const;

    // Reads a JSON array param as a set of strings, skipping any non-string elements. The shared
    // primitive behind the hand-rolled "JSON string array -> collection" loops (e.g. the nodeIds
    // filter in get_graph_connections / describe_metasound). An absent or non-array param yields an
    // empty set. Values are taken verbatim — no trim/case-fold — so callers that need normalization
    // (e.g. ReadFieldProjection) layer it on top of the raw GetArray instead.
    TSet<FString> GetStringSet(const FString& Key) const;

    // Parses the shared per-row "fields"/"namesOnly" column-projection convention
    // used by list-style handlers (actor.list, system.console.search). An explicit
    // `fields` allow-list (a JSON array of strings, or a bare `fields`/`field`
    // string) wins; otherwise a truthy `namesOnly`/`names_only` expands to the
    // caller-supplied NamesOnlyKeys. Returns the wanted-key set lowercased and
    // trimmed; an empty set means "no projection" (emit every column). The
    // per-handler difference — which columns namesOnly expands to — is the
    // NamesOnlyKeys argument, so the parse-and-resolve logic lives in one place.
    // Pass NamesOnlyKeys already lowercased to match the lowercased lookups.
    TSet<FString> ReadFieldProjection(const TArray<FString>& NamesOnlyKeys) const;

    // Response helpers
    void SendSuccess(const TSharedPtr<FJsonObject>& Result) const;
    void SendSuccess(const FString& Message) const;
    void SendSuccess(const FString& Message, const TSharedPtr<FJsonObject>& Result) const;
    void SendError(const FString& ErrorCode, const FString& Message) const;
    void SendError(const FString& ErrorCode, const FString& Message, const TSharedPtr<FJsonObject>& Result) const;

    // Standardized rejection for a method whose feature requires a newer Unreal Engine than the
    // running editor. Sends error code "UNSUPPORTED_ENGINE_VERSION" naming the required minimum
    // and the current engine version. RequiredVersion e.g. TEXT("5.6"); Feature e.g.
    // TEXT("Adding MetaSound graph variables").
    void SendUnsupportedEngineVersion(const FString& RequiredVersion, const FString& Feature) const;

    // Long-running job helper: allocates a ticket, sends the immediate running response,
    // then binds the native completion delegate. Returns the ticket ID.
    // Streaming opt-in: when the request carries wait=true AND arrived on a
    // streaming transport connection (Subsystem->IsStreamingRequest), the
    // immediate ticket response is suppressed and the ticket is registered with
    // the subsystem's job-event bridge instead, which resolves the HTTP request
    // on the job's terminal event.
    FString StartJob(const FJobBindArgs& Args) const;

    // Builds a shareable late-response handle for handlers that finish inside an
    // AsyncTask / next-tick lambda. Capture the returned TSharedRef by value and
    // call Token->SendSuccess / Token->SendError from the lambda instead of the
    // subsystem's response APIs, so the response still flows through the
    // capture path (test fixtures, text formatters).
    TSharedRef<FAsyncResponseToken> MakeAsyncToken() const;

    // Retain only the dispatcher's lifetime, not its active-request guard. The
    // callback runs exactly once if the dispatcher ends before Release. Returns
    // null for standalone contexts that have no dispatcher.
    TSharedPtr<FAsyncRequestLifetimeLease> RetainAsyncRequestLifetime(
        TFunction<void()> OnOwnerAbandoned) const;

    // Preserve the dispatcher's active-request scope when safe-point work has to
    // outlive this stack. The optional callback runs instead of Work if the
    // dispatcher ends first. Returns false for standalone contexts with no dispatcher.
    bool DeferActiveRequestToSafePoint(
        TFunction<void()> Work, const TCHAR* Reason,
        TFunction<void()> OnOwnerAbandoned = TFunction<void()>()) const;

    // Access raw data
    const FString& GetRequestId() const;
    const FString& GetMethod() const;
    const TSharedPtr<FJsonObject>& GetRawPayload() const;

    // Access subsystem (for handlers that need UE subsystem APIs)
    UPinWrightSubsystem* GetSubsystem() const;

    // Factory used by tests to build a context without dispatcher plumbing.
    static FHandlerContext MakeTestContext(
        const FString& InRequestId,
        const FString& InMethod,
        const TSharedPtr<FJsonObject>& InPayload,
        UPinWrightSubsystem* InSubsystem = nullptr);

#if WITH_DEV_AUTOMATION_TESTS
    // Factory that captures SendSuccess/SendError responses for test assertions.
    static FHandlerContext MakeTestContextWithCapture(
        const FString& InRequestId,
        const FString& InMethod,
        const TSharedPtr<FJsonObject>& InPayload,
        FTestResponseCapture* Capture);
#endif

    // Factory used by the dispatcher when a text formatter is registered: the
    // captured response is intercepted and post-processed before being sent
    // back through the subsystem/transport.
    static FHandlerContext MakeContextWithCapture(
        const FString& InRequestId,
        const FString& InMethod,
        const TSharedPtr<FJsonObject>& InPayload,
        UPinWrightSubsystem* InSubsystem,
        FResponseCapture* Capture);

#if WITH_DEV_AUTOMATION_TESTS
    // Factory for test fixtures that drive an *async* handler (one that finishes
    // inside an AsyncTask / next-tick lambda via MakeAsyncToken). The capture is
    // shared-owned so the async token can hold a weak handle and safely no-op if
    // the test's capture is destroyed before the lambda runs. Use this instead of
    // the raw-pointer overload whenever the handler under test is async.
    static FHandlerContext MakeTestContextWithSharedCapture(
        const FString& InRequestId,
        const FString& InMethod,
        const TSharedPtr<FJsonObject>& InPayload,
        const TSharedRef<FResponseCapture>& Capture);

    void SetDispatcherForTesting(FRpcDispatcher* InDispatcher) { Dispatcher = InDispatcher; }
#endif

private:
    friend class UPinWrightSubsystem;
    friend class FRpcDispatcher; // constructed by dispatcher during DrainAutoRegistrations
    FString RequestId;
    FString Method;
    TSharedPtr<FJsonObject> Payload;
    UPinWrightSubsystem* Subsystem = nullptr;
    FRpcDispatcher* Dispatcher = nullptr;
    FResponseCapture* ResponseCapture = nullptr;
    // Shared-owned alias of ResponseCapture, set only when the capture is owned
    // via TSharedPtr (async-capable test fixtures). MakeAsyncToken hands the
    // resulting weak handle to the token so a late async completion can't write
    // through a freed capture. Empty for the synchronous raw-pointer path.
    TWeakPtr<FResponseCapture> WeakResponseCapture;
};
