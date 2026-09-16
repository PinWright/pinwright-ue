// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

class FScopedTransaction;
class UObject;

namespace PinWrightTransactionUtils
{
    void ApplyAndCancelTransaction(FScopedTransaction& Transaction);
    void PrepareTransactionalSnapshot(UObject* Object);
    bool RollbackLastTransaction();
}
