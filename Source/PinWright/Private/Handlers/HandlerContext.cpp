// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerContext.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "State/PluginState.h"
#include "State/JobRegistry.h"
#include "Utils/JobMonitorLog.h"
#include "Utils/JsonUtils.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersionComparison.h"  // ENGINE_MAJOR/MINOR_VERSION for version-rejection messages

// ---------------------------------------------------------------------------
// Typed param getters
// ---------------------------------------------------------------------------

FString FHandlerContext::GetString(const FString& Key, const FString& Default) const
{
    if (Payload.IsValid() && Payload->HasField(Key))
    {
        return Payload->GetStringField(Key);
    }
    return Default;
}

double FHandlerContext::GetNumber(const FString& Key, double Default) const
{
    if (Payload.IsValid() && Payload->HasField(Key))
    {
        return Payload->GetNumberField(Key);
    }
    return Default;
}

bool FHandlerContext::GetBool(const FString& Key, bool Default) const
{
    if (Payload.IsValid() && Payload->HasField(Key))
    {
        return Payload->GetBoolField(Key);
    }
    return Default;
}

int32 FHandlerContext::GetInt(const FString& Key, int32 Default) const
{
    const TOptional<int32> Parsed = GetIntOr(Key);
    return Parsed.IsSet() ? Parsed.GetValue() : Default;
}

TOptional<int32> FHandlerContext::GetIntOr(const FString& Key) const
{
    if (!Payload.IsValid() || !Payload->HasField(Key))
    {
        return TOptional<int32>();
    }

    int64 ParsedValue = 0;
    FString ParseError;
    if (!TryParseStrictJsonInteger(
            Payload->TryGetField(Key),
            static_cast<int64>(TNumericLimits<int32>::Min()),
            static_cast<int64>(TNumericLimits<int32>::Max()),
            ParsedValue,
            ParseError))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Field '%s' must be a finite in-range integer: %s"),
                *Key, *ParseError));
        return TOptional<int32>();
    }

    return TOptional<int32>(static_cast<int32>(ParsedValue));
}

FVector FHandlerContext::GetVector(const FString& Key, FVector Default) const
{
    if (!Payload.IsValid())
    {
        return Default;
    }
    return ExtractVectorField(Payload, *Key, Default);
}

FRotator FHandlerContext::GetRotator(const FString& Key, FRotator Default) const
{
    if (!Payload.IsValid())
    {
        return Default;
    }
    return ExtractRotatorField(Payload, *Key, Default);
}

TSharedPtr<FJsonObject> FHandlerContext::GetObject(const FString& Key) const
{
    if (Payload.IsValid() && Payload->HasTypedField<EJson::Object>(Key))
    {
        return Payload->GetObjectField(Key);
    }
    return nullptr;
}

const TArray<TSharedPtr<FJsonValue>>* FHandlerContext::GetArray(const FString& Key) const
{
    if (Payload.IsValid() && Payload->HasTypedField<EJson::Array>(Key))
    {
        return &Payload->GetArrayField(Key);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Require + validate
// ---------------------------------------------------------------------------

bool FHandlerContext::RequireString(const FString& Key, FString& Out) const
{
    if (!Payload.IsValid() || !Payload->HasField(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required string field: %s"), *Key));
        return false;
    }

    Out = Payload->GetStringField(Key);
    if (Out.IsEmpty())
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required string field: %s"), *Key));
        return false;
    }

    return true;
}

bool FHandlerContext::RequireAssetPath(const FString& Key, FString& Out) const
{
    FString RawPath;
    if (!RequireString(Key, RawPath))
    {
        return false;
    }

    FString Sanitized = SanitizeProjectRelativePath(RawPath);
    if (Sanitized.IsEmpty() || !IsValidAssetPath(Sanitized))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Invalid asset path for field '%s': %s"), *Key, *RawPath));
        return false;
    }

    Out = Sanitized;
    return true;
}

bool FHandlerContext::RequireAssetPath(const TArray<FString>& Keys, FString& Out) const
{
    const FString CanonicalKey = Keys.Num() > 0 ? Keys[0] : FString(TEXT("assetPath"));

    FString RawPath = GetStringFirstOf(Keys);
    if (RawPath.IsEmpty())
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required string field: %s"), *CanonicalKey));
        return false;
    }

    FString Sanitized = SanitizeProjectRelativePath(RawPath);
    if (Sanitized.IsEmpty() || !IsValidAssetPath(Sanitized))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Invalid asset path for field '%s': %s"), *CanonicalKey, *RawPath));
        return false;
    }

    Out = Sanitized;
    return true;
}

bool FHandlerContext::RequireInt(const FString& Key, int32& Out) const
{
    if (!Payload.IsValid() || !Payload->HasField(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required integer field: %s"), *Key));
        return false;
    }

    int64 ParsedValue = 0;
    FString ParseError;
    if (!TryParseStrictJsonInteger(
            Payload->TryGetField(Key),
            static_cast<int64>(TNumericLimits<int32>::Min()),
            static_cast<int64>(TNumericLimits<int32>::Max()),
            ParsedValue,
            ParseError))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Field '%s' must be a finite in-range integer: %s"),
                *Key, *ParseError));
        return false;
    }

    Out = static_cast<int32>(ParsedValue);
    return true;
}

bool FHandlerContext::RequireNumber(const FString& Key, double& Out) const
{
    if (!Payload.IsValid() || !Payload->HasField(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required number field: %s"), *Key));
        return false;
    }

    if (!Payload->HasTypedField<EJson::Number>(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Field '%s' is not a number"), *Key));
        return false;
    }

    Out = Payload->GetNumberField(Key);
    return true;
}

bool FHandlerContext::RequireBool(const FString& Key, bool& Out) const
{
    if (!Payload.IsValid() || !Payload->HasField(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required boolean field: %s"), *Key));
        return false;
    }

    if (!Payload->HasTypedField<EJson::Boolean>(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Field '%s' is not a boolean"), *Key));
        return false;
    }

    Out = Payload->GetBoolField(Key);
    return true;
}

bool FHandlerContext::RequireObject(const FString& Key, TSharedPtr<FJsonObject>& Out) const
{
    if (!Payload.IsValid() || !Payload->HasField(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required object field: %s"), *Key));
        return false;
    }

    if (!Payload->HasTypedField<EJson::Object>(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Field '%s' is not an object"), *Key));
        return false;
    }

    Out = Payload->GetObjectField(Key);
    return true;
}

bool FHandlerContext::RequireArray(const FString& Key, const TArray<TSharedPtr<FJsonValue>>*& Out) const
{
    if (!Payload.IsValid() || !Payload->HasField(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Missing required array field: %s"), *Key));
        return false;
    }

    if (!Payload->HasTypedField<EJson::Array>(Key))
    {
        SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Field '%s' is not an array"), *Key));
        return false;
    }

    Out = &Payload->GetArrayField(Key);
    return true;
}

// ---------------------------------------------------------------------------
// Multi-key alias lookup
// ---------------------------------------------------------------------------

FString FHandlerContext::GetStringFirstOf(const TArray<FString>& Keys, const FString& Default) const
{
    for (const FString& Key : Keys)
    {
        FString Value = GetString(Key);
        if (!Value.IsEmpty())
        {
            return Value;
        }
    }
    return Default;
}

bool FHandlerContext::GetBoolFirstOf(const TArray<FString>& Keys, bool Default) const
{
    if (Payload.IsValid())
    {
        for (const FString& Key : Keys)
        {
            if (Payload->HasField(Key))
            {
                return Payload->GetBoolField(Key);
            }
        }
    }
    return Default;
}

TOptional<int32> FHandlerContext::GetIntFirstOf(const TArray<FString>& Keys) const
{
    if (Payload.IsValid())
    {
        for (const FString& Key : Keys)
        {
            if (Payload->HasField(Key))
            {
                return GetIntOr(Key);
            }
        }
    }
    return TOptional<int32>();
}

TSharedPtr<FJsonValue> FHandlerContext::GetJsonValueFirstOf(const TArray<FString>& Keys) const
{
    if (!Payload.IsValid())
    {
        return nullptr;
    }
    for (const FString& Key : Keys)
    {
        if (TSharedPtr<FJsonValue> Value = Payload->TryGetField(Key))
        {
            return Value;
        }
    }
    return nullptr;
}

TSet<FString> FHandlerContext::GetStringSet(const FString& Key) const
{
    TSet<FString> Result;
    if (const TArray<TSharedPtr<FJsonValue>>* Array = GetArray(Key))
    {
        for (const TSharedPtr<FJsonValue>& Value : *Array)
        {
            if (Value.IsValid() && Value->Type == EJson::String)
            {
                Result.Add(Value->AsString());
            }
        }
    }
    return Result;
}

TSet<FString> FHandlerContext::ReadFieldProjection(const TArray<FString>& NamesOnlyKeys) const
{
    TSet<FString> Fields;
    auto AddField = [&Fields](const FString& Raw)
    {
        FString F = Raw;
        F.TrimStartAndEndInline();
        if (!F.IsEmpty())
        {
            Fields.Add(F.ToLower());
        }
    };

    // An explicit `fields` array wins; a bare `fields`/`field` string is the
    // single-key shorthand; only when no fields were supplied does namesOnly
    // expand to the caller's column set.
    if (const TArray<TSharedPtr<FJsonValue>>* FieldArray = GetArray(TEXT("fields")))
    {
        for (const TSharedPtr<FJsonValue>& Value : *FieldArray)
        {
            FString F;
            if (Value.IsValid() && Value->TryGetString(F))
            {
                AddField(F);
            }
        }
    }
    else
    {
        const FString SingleField = GetStringFirstOf({TEXT("fields"), TEXT("field")});
        if (!SingleField.IsEmpty())
        {
            AddField(SingleField);
        }
    }

    if (Fields.Num() == 0 &&
        GetBoolFirstOf({TEXT("namesOnly"), TEXT("names_only")}, false))
    {
        for (const FString& Key : NamesOnlyKeys)
        {
            AddField(Key);
        }
    }
    return Fields;
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

void FHandlerContext::SendSuccess(const TSharedPtr<FJsonObject>& Result) const
{
    if (ResponseCapture)
    {
        ResponseCapture->bWasCalled = true;
        ++ResponseCapture->CallCount;
        ResponseCapture->bSuccess = true;
        ResponseCapture->Result = Result;
        // A success message rides in the result's "message" field, written either by the
        // SendSuccess(Message, Result) overload below or by the handler itself. Reading it
        // back here makes this the one writer of Capture.Message on the success path, so a
        // capture cannot report a handler as having said nothing when it named its outcome
        // (rpc-design.md rule 1). Left untouched when there is no message to read.
        if (Result.IsValid())
        {
            Result->TryGetStringField(TEXT("message"), ResponseCapture->Message);
        }
        return;
    }
    if (Subsystem)
    {
        Subsystem->SendAutomationResponse(RequestId, /*bSuccess=*/true, TEXT(""), Result);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("HandlerContext::SendSuccess called with no Subsystem and no ResponseCapture — response dropped"));
    }
}

void FHandlerContext::SendSuccess(const FString& Message) const
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), Message);
    SendSuccess(Result);
}

void FHandlerContext::SendSuccess(const FString& Message, const TSharedPtr<FJsonObject>& Result) const
{
    if (Result.IsValid())
    {
        Result->SetStringField(TEXT("message"), Message);
    }
    SendSuccess(Result);
}

void FHandlerContext::SendError(const FString& ErrorCode, const FString& Message) const
{
    SendError(ErrorCode, Message, nullptr);
}

void FHandlerContext::SendError(
    const FString& ErrorCode,
    const FString& Message,
    const TSharedPtr<FJsonObject>& Result) const
{
    if (ResponseCapture)
    {
        ResponseCapture->bWasCalled = true;
        ++ResponseCapture->CallCount;
        ResponseCapture->bSuccess = false;
        ResponseCapture->ErrorCode = ErrorCode;
        ResponseCapture->Message = Message;
        ResponseCapture->Result = Result;
        return;
    }
    if (Subsystem)
    {
        if (Result.IsValid())
        {
            const FString ResolvedError =
                ErrorCode.IsEmpty() ? FString(ErrorCodes::ERR_AUTOMATION_ERROR) : ErrorCode;
            Subsystem->SendAutomationResponse(RequestId, false, Message, Result, ResolvedError);
        }
        else
        {
            Subsystem->SendAutomationError(RequestId, Message, ErrorCode);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("HandlerContext::SendError called with no Subsystem and no ResponseCapture — response dropped"));
    }
}

// ---------------------------------------------------------------------------
// Async response token
// ---------------------------------------------------------------------------

void FAsyncResponseToken::SendSuccess(const TSharedPtr<FJsonObject>& Result) const
{
    // Pin the capture: if the originating fixture's capture was already freed,
    // Pin() returns null and we fall through to the subsystem path (or drop) —
    // never dereferencing a dangling pointer.
    if (TSharedPtr<FResponseCapture> Capture = WeakResponseCapture.Pin())
    {
        Capture->bWasCalled = true;
        ++Capture->CallCount;
        Capture->bSuccess = true;
        Capture->Result = Result;
        // Same one-writer rule as FHandlerContext::SendSuccess: the message the handler
        // sent is read off the result so an async completion records it too.
        if (Result.IsValid())
        {
            Result->TryGetStringField(TEXT("message"), Capture->Message);
        }
        return;
    }
    if (UPinWrightSubsystem* Sub = WeakSubsystem.Get())
    {
        Sub->SendAutomationResponse(RequestId, /*bSuccess=*/true, TEXT(""), Result);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("FAsyncResponseToken::SendSuccess called with no Subsystem and no live ResponseCapture — response dropped"));
    }
}

void FAsyncResponseToken::SendSuccess(const FString& Message, const TSharedPtr<FJsonObject>& Result) const
{
    if (Result.IsValid())
    {
        Result->SetStringField(TEXT("message"), Message);
    }
    SendSuccess(Result);
}

void FAsyncResponseToken::SendError(const FString& ErrorCode, const FString& Message) const
{
    SendError(ErrorCode, Message, nullptr);
}

void FAsyncResponseToken::SendError(
    const FString& ErrorCode,
    const FString& Message,
    const TSharedPtr<FJsonObject>& Result) const
{
    if (TSharedPtr<FResponseCapture> Capture = WeakResponseCapture.Pin())
    {
        Capture->bWasCalled = true;
        ++Capture->CallCount;
        Capture->bSuccess = false;
        Capture->ErrorCode = ErrorCode;
        Capture->Message = Message;
        Capture->Result = Result;
        return;
    }
    if (UPinWrightSubsystem* Sub = WeakSubsystem.Get())
    {
        if (Result.IsValid())
        {
            const FString ResolvedError =
                ErrorCode.IsEmpty() ? FString(ErrorCodes::ERR_AUTOMATION_ERROR) : ErrorCode;
            Sub->SendAutomationResponse(RequestId, false, Message, Result, ResolvedError);
        }
        else
        {
            Sub->SendAutomationError(RequestId, Message, ErrorCode);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("FAsyncResponseToken::SendError called with no Subsystem and no live ResponseCapture — response dropped"));
    }
}

TSharedRef<FAsyncResponseToken> FHandlerContext::MakeAsyncToken() const
{
    TSharedRef<FAsyncResponseToken> Token = MakeShared<FAsyncResponseToken>();
    Token->RequestId = RequestId;
    Token->Method = Method;
    Token->Payload = Payload;
    Token->WeakSubsystem = Subsystem;
    // Only propagate a *weak* handle to a shared-owned capture. The raw
    // ResponseCapture pointer (synchronous dispatcher/text-format path) is
    // deliberately not forwarded: a synchronous capture is stack-owned by the
    // caller and would dangle the moment the async lambda outlives it. When the
    // capture is shared-owned (async-capable test fixtures), WeakResponseCapture
    // is set and the token can safely write through it — or no-op if it's gone.
    Token->WeakResponseCapture = WeakResponseCapture;
    return Token;
}

FAsyncRequestLifetimeLease::~FAsyncRequestLifetimeLease()
{
    Release();
}

void FAsyncRequestLifetimeLease::Release()
{
    if (!bActive)
    {
        return;
    }

    bActive = false;
    OnOwnerAbandoned = TFunction<void()>();
    TFunction<void()> Release = MoveTemp(ReleaseRegistration);
    if (Release)
    {
        Release();
    }
}

void FAsyncRequestLifetimeLease::Abandon()
{
    if (!bActive)
    {
        return;
    }

    bActive = false;
    ReleaseRegistration = TFunction<void()>();
    TFunction<void()> Callback = MoveTemp(OnOwnerAbandoned);
    if (Callback)
    {
        Callback();
    }
}

TSharedPtr<FAsyncRequestLifetimeLease> FHandlerContext::RetainAsyncRequestLifetime(
    TFunction<void()> OnOwnerAbandoned) const
{
    return Dispatcher
        ? Dispatcher->RetainAsyncRequestLifetime(MoveTemp(OnOwnerAbandoned))
        : nullptr;
}

bool FHandlerContext::DeferActiveRequestToSafePoint(
    TFunction<void()> Work, const TCHAR* Reason,
    TFunction<void()> OnOwnerAbandoned) const
{
    return Dispatcher && Dispatcher->DeferActiveRequestToSafePoint(
        MoveTemp(Work), Reason, MoveTemp(OnOwnerAbandoned));
}

void FHandlerContext::SendUnsupportedEngineVersion(const FString& RequiredVersion, const FString& Feature) const
{
    SendError(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION,
        FString::Printf(TEXT("%s requires Unreal Engine %s or newer (this editor is %d.%d)."),
            *Feature, *RequiredVersion, ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
}

// ---------------------------------------------------------------------------
// Raw data accessors
// ---------------------------------------------------------------------------

const FString& FHandlerContext::GetRequestId() const
{
    return RequestId;
}

const FString& FHandlerContext::GetMethod() const
{
    return Method;
}

const TSharedPtr<FJsonObject>& FHandlerContext::GetRawPayload() const
{
    return Payload;
}

UPinWrightSubsystem* FHandlerContext::GetSubsystem() const
{
    return Subsystem;
}

// ---------------------------------------------------------------------------
// Long-running job helper
// ---------------------------------------------------------------------------

FString FHandlerContext::StartJob(const FJobBindArgs& Args) const
{
    FJobRegistry& Reg = FPluginState::Get().GetJobRegistry();

    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    if (Payload.IsValid())
    {
        Params = Payload.ToSharedRef();
    }
    const FString TicketId = Reg.Start(Args.Method, Params);

    // Streaming opt-in: IsStreamingRequest is true only when the transport's
    // triple gate held (progressToken + Accept: text/event-stream + wait=true;
    // the dispatcher strips "wait" as an internal param before handlers see it,
    // so it must not be re-read here). Streaming suppresses the immediate
    // ticket response — the subsystem's job-event bridge resolves the HTTP
    // request when the job reaches a terminal state. The registration must land
    // after FJobRegistry::Start and BEFORE BindNativeDelegate so an
    // immediately-completing job cannot race it.
    UPinWrightSubsystem* Sub = GetSubsystem();
    const bool bStreamJob = Sub && Sub->IsStreamingRequest(RequestId);
    if (bStreamJob)
    {
        Sub->RegisterStreamingJob(TicketId, RequestId);
    }
    else
    {
        auto Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("running"));
        Resp->SetStringField(TEXT("ticket_id"), TicketId);
        Resp->SetStringField(TEXT("monitor_path"), JobMonitorLog::JobsJsonlRelativePath);
        Resp->SetStringField(TEXT("method"), Args.Method);
        Resp->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
        Resp->SetStringField(TEXT("docs"), TEXT("call(\"system.job_status\") for guidance"));
        if (Args.StartedPayload.IsValid())
        {
            for (const auto& Pair : Args.StartedPayload->Values)
                Resp->SetField(Pair.Key, Pair.Value);
        }
        SendSuccess(Resp);
    }

    if (Args.BindNativeDelegate)
    {
        FJobOnComplete OnComplete =
            [TicketId](bool bSuccess, TSharedPtr<FJsonObject> Result, FString Error)
        {
            FPluginState::Get().GetJobRegistry()
                .Complete(TicketId, bSuccess, Result, Error);
        };
        Args.BindNativeDelegate(MoveTemp(OnComplete));
    }
    return TicketId;
}

FHandlerContext FHandlerContext::MakeTestContext(
    const FString& InRequestId,
    const FString& InMethod,
    const TSharedPtr<FJsonObject>& InPayload,
    UPinWrightSubsystem* InSubsystem)
{
    FHandlerContext Ctx;
    Ctx.RequestId = InRequestId;
    Ctx.Method = InMethod;
    Ctx.Payload = InPayload;
    Ctx.Subsystem = InSubsystem;
    return Ctx;
}

#if WITH_DEV_AUTOMATION_TESTS
FHandlerContext FHandlerContext::MakeTestContextWithCapture(
    const FString& InRequestId,
    const FString& InMethod,
    const TSharedPtr<FJsonObject>& InPayload,
    FTestResponseCapture* Capture)
{
    FHandlerContext Ctx;
    Ctx.RequestId = InRequestId;
    Ctx.Method = InMethod;
    Ctx.Payload = InPayload;
    Ctx.Subsystem = nullptr;
    Ctx.ResponseCapture = Capture;
    return Ctx;
}

FHandlerContext FHandlerContext::MakeTestContextWithSharedCapture(
    const FString& InRequestId,
    const FString& InMethod,
    const TSharedPtr<FJsonObject>& InPayload,
    const TSharedRef<FResponseCapture>& Capture)
{
    FHandlerContext Ctx;
    Ctx.RequestId = InRequestId;
    Ctx.Method = InMethod;
    Ctx.Payload = InPayload;
    Ctx.Subsystem = nullptr;
    // Wire both the raw pointer (synchronous capture path) and the weak handle
    // (async token path) to the same shared-owned capture.
    Ctx.ResponseCapture = &Capture.Get();
    Ctx.WeakResponseCapture = Capture.ToWeakPtr();
    return Ctx;
}
#endif

FHandlerContext FHandlerContext::MakeContextWithCapture(
    const FString& InRequestId,
    const FString& InMethod,
    const TSharedPtr<FJsonObject>& InPayload,
    UPinWrightSubsystem* InSubsystem,
    FResponseCapture* Capture)
{
    FHandlerContext Ctx;
    Ctx.RequestId = InRequestId;
    Ctx.Method = InMethod;
    Ctx.Payload = InPayload;
    Ctx.Subsystem = InSubsystem;
    Ctx.ResponseCapture = Capture;
    return Ctx;
}
