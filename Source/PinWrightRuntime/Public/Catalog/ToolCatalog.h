// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FRpcDispatcher;

class PINWRIGHTRUNTIME_API FToolCatalog
{
public:
    void Initialize(TSharedPtr<FRpcDispatcher> InDispatcher);

private:
    TWeakPtr<FRpcDispatcher> DispatcherWeak;
};
