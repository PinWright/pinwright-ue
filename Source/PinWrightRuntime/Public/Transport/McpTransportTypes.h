// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Shared transport types used by the socket HTTP server (FSocketHttpServer)
// and the dispatcher/subsystem wiring.

// Completion callback signature: (RequestId, bSuccess, Message, Result, ErrorCode)
using FTransportCompletionCallback = TFunction<void(
    const FString&, bool, const FString&,
    const TSharedPtr<FJsonObject>&, const FString&)>;

// Delegate fired when a valid execute request arrives (dispatcher binds to this).
// Params: RequestId, DispatchKey (method or resolved tool key), Params JSON
DECLARE_DELEGATE_ThreeParams(FOnRpcRequest,
    const FString& /*RequestId*/,
    const FString& /*Method*/,
    const TSharedPtr<FJsonObject>& /*Params*/);
