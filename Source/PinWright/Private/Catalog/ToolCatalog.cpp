// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Catalog/ToolCatalog.h"
#include "Dispatch/RpcDispatcher.h"

DECLARE_LOG_CATEGORY_EXTERN(LogToolCatalog, Log, All);
DEFINE_LOG_CATEGORY(LogToolCatalog);

void FToolCatalog::Initialize(TSharedPtr<FRpcDispatcher> InDispatcher)
{
    DispatcherWeak = InDispatcher;
}
