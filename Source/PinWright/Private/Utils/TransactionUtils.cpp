// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/TransactionUtils.h"


#include "Editor.h"
#include "Editor/Transactor.h"
#include "Editor/UnrealEdEngine.h"
#include "ScopedTransaction.h"
#include "UObject/Object.h"
#include "UnrealEdGlobals.h"

namespace PinWrightTransactionUtils
{
    // Replays the active scoped transaction's recorded property diffs in reverse and
    // drops the transaction record so the undo buffer is unchanged at exit.
    //
    // Implementation notes — why we go through UTransBuffer::Undo instead of
    // calling FTransaction::Apply() directly:
    //
    // The engine's UEdGraphPin pin serializer asks GEditor->Trans->IsObjectTransacting()
    // during FTransaction::Apply to decide how to deduplicate LinkedTo entries.
    // IsObjectTransacting reads UTransBuffer::CurrentTransaction, which is set
    // ONLY inside UTransBuffer::Undo / Redo. Calling FTransaction::Apply directly
    // (the previous implementation) leaves CurrentTransaction null, fires
    // ensure(CurrentTransaction) in UTransBuffer::IsObjectTransacting and again as
    // ensure(!Pin->LinkedTo.Contains(ReferencingPin)) in UEdGraphPin::SerializePin
    // because the dedupe path is unreachable. The visible symptom is duplicate
    // exec-pin links on rolled-back nodes.
    //
    // UTransBuffer::Undo sets CurrentTransaction correctly for the duration of
    // Apply, but it also broadcasts UndoDelegate, which UEditorEngine routes to
    // BroadcastPostUndoRedo → FEditorUndoClient::PostUndo on every registered
    // client. The Blueprint editor module's client reacts by calling
    // FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified, kicking a
    // synchronous skeleton compile that ensures(SkeletonCompiledBlueprints.Num()==1)
    // and freezes the game thread and HTTP transport.
    //
    // bSuspendBroadcastPostUndoRedo gates the BroadcastPostUndoRedo call inside
    // UEditorEngine::HandleTransactorRedoUndo. With it set we get the
    // CurrentTransaction-aware Apply path without the structural-compile cascade.
    // bSquelchTransactionNotification suppresses the user-facing toast.
    //
    // Caller invariant: must be invoked while `Transaction` is still in scope
    // and outstanding. We End the transaction explicitly (closes ActiveCount
    // and finalizes the record) before invoking Undo, then let
    // FScopedTransaction's destructor see ActiveCount==0 and no-op out.
    void ApplyAndCancelTransaction(FScopedTransaction& Transaction)
    {
        if (!GEditor || !GEditor->Trans || !GUnrealEd || !GUndo)
        {
            // Fall back to FScopedTransaction's own cancel path if the editor
            // transactor is missing entirely (test harness / commandlet edge).
            Transaction.Cancel();
            return;
        }

        const FGuid TransactionId = GUndo->GetContext().TransactionId;

        // Close the transaction so CanUndo() returns true. EndTransaction
        // finalizes the record and leaves non-transient records on the top of
        // the undo buffer; ActiveCount→0, GUndo→nullptr. FScopedTransaction's
        // destructor will call EndTransaction again later, but UTransBuffer::End
        // is idempotent at ActiveCount==0.
        GEditor->EndTransaction();

        const FTransactionContext UndoContext = GEditor->Trans->GetUndoContext(/*bCheckWhetherUndoPossible=*/false);
        if (UndoContext.TransactionId != TransactionId)
        {
            return;
        }

        // Suppress the post-undo broadcast so the Blueprint editor's undo
        // client never runs MarkBlueprintAsStructurallyModified during Apply.
        // Save/restore manually because bitfield members can't bind to TGuardValue.
        const uint32 PrevSuspendBroadcast = GUnrealEd->bSuspendBroadcastPostUndoRedo;
        const uint32 PrevSquelchNotif = GUnrealEd->bSquelchTransactionNotification;
        GUnrealEd->bSuspendBroadcastPostUndoRedo = 1;
        GUnrealEd->bSquelchTransactionNotification = 1;

        // bCanRedo=false: pop the transaction off the undo buffer afterward
        // so users don't see a phantom "Undo MCP: blueprint.compile_bpir" entry.
        GEditor->Trans->Undo(/*bCanRedo=*/false);

        GUnrealEd->bSuspendBroadcastPostUndoRedo = PrevSuspendBroadcast;
        GUnrealEd->bSquelchTransactionNotification = PrevSquelchNotif;
    }

    void PrepareTransactionalSnapshot(UObject* Object)
    {
        if (!Object)
        {
            return;
        }

        Object->SetFlags(RF_Transactional);
        Object->Modify();
    }

    bool RollbackLastTransaction()
    {
        if (!GEditor)
        {
            return false;
        }

        return GEditor->UndoTransaction(/*bCanRedo=*/false);
    }
}
