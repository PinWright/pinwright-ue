// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared dispatcher fixture helpers for tests that route requests through
// FRpcDispatcher::ProcessRequest with a response-capturing sink. Extracted from
// anonymous namespaces previously duplicated across TestMaterialDispatcherAliases.cpp
// and RecorderListSessionsLimitTest.cpp. Required because the plugin's tests share a
// single module with Unity builds: anonymous-namespace helpers with the same name
// across .cpp files produce ODR / redefinition errors when Unity merges them into
// one translation unit.
//
// Conventions match Tests/Material/MaterialEditorOpenTestHelpers.h: named namespace
// + inline functions, no module API macro.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"

namespace DispatcherTestHelpers
{
    // Captured dispatcher response. Replaces the legacy FMcpTransport completion
    // registry: the dispatcher's FResponseSink writes the most recent response here.
    struct FSinkCapture
    {
        bool bWasCalled = false;
        bool bSuccess = false;
        FString Message;
        TSharedPtr<FJsonObject> Result;
        FString ErrorCode;
    };

    using FSinkPtr = TSharedPtr<FSinkCapture>;

    // Build a dispatcher whose response sink records into OutSink, draining the
    // production auto-registrations with a null subsystem so responses flow back
    // through the captured-response path. Initialize() must precede
    // DrainAutoRegistrations() because the handler bridge lambdas capture the
    // sink by value at drain time.
    inline void MakeDispatcher(FSinkPtr& OutSink, FRpcDispatcher& Dispatcher)
    {
        OutSink = MakeShared<FSinkCapture>();
        FSinkPtr Sink = OutSink;
        Dispatcher.Initialize(FResponseSink(
            [Sink](const FString& /*RequestId*/, bool bSuccess, const FString& Message,
                   const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
            {
                Sink->bWasCalled = true;
                Sink->bSuccess = bSuccess;
                Sink->Message = Message;
                Sink->Result = Result;
                Sink->ErrorCode = ErrorCode;
            }));
        Dispatcher.DrainAutoRegistrations(nullptr);
    }

    // Dispatch one request and capture the (bSuccess, result, errorCode) outcome.
    inline void Dispatch(FRpcDispatcher& Dispatcher, FSinkPtr& Sink,
        const FString& Method, const FString& RequestId,
        const TSharedPtr<FJsonObject>& Params, bool& bOutSuccess,
        TSharedPtr<FJsonObject>& OutResult, FString& OutErrorCode)
    {
        *Sink = FSinkCapture();
        Dispatcher.ProcessRequest(RequestId, Method, Params);
        bOutSuccess = Sink->bSuccess;
        OutResult = Sink->Result;
        OutErrorCode = Sink->ErrorCode;
    }

    // Dispatch one request and capture the (bSuccess, errorCode) outcome, discarding the result.
    inline void Dispatch(FRpcDispatcher& Dispatcher, FSinkPtr& Sink,
        const FString& Method, const FString& RequestId,
        const TSharedPtr<FJsonObject>& Params, bool& bOutSuccess, FString& OutErrorCode)
    {
        TSharedPtr<FJsonObject> DiscardedResult;
        Dispatch(Dispatcher, Sink, Method, RequestId, Params,
            bOutSuccess, DiscardedResult, OutErrorCode);
    }
}
