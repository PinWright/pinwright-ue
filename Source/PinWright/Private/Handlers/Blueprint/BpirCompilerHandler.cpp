// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirCompilerHandler.cpp - Compile BPIR code into Blueprint nodes

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Utils/AssetUtils.h"
#include "Utils/BlueprintGraphSnapshot.h"
#include "Utils/PieState.h"
#include "Utils/TransactionUtils.h"
#include "Compat/JsonKeyCompat.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace BlueprintHandlerUtils;

// Parses the optional "context" object param and injects external variable references
// into the compiler. Each key is a variable name, each value is "nodeGuid:pinName".
// Returns false and sends an error via Ctx if any resolution fails.
static bool ResolveContextInjections(FHandlerContext& Ctx, UBlueprint* BP, FBpirCompiler& Compiler)
{
    TSharedPtr<FJsonObject> ContextObj = Ctx.GetObject(TEXT("context"));
    if (!ContextObj.IsValid())
    {
        return true;
    }

    for (const auto& Pair : ContextObj->Values)
    {
        const FString VarName = EARGCompat::JsonKeyToString(Pair.Key);
        FString Value = Pair.Value->AsString();
        if (Value.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_CONTEXT,
                FString::Printf(TEXT("Context variable '%s' has empty value"), *VarName));
            return false;
        }

        // Split on last ':' — GUIDs use '-', so the only ':' is our separator
        int32 ColonIdx = INDEX_NONE;
        Value.FindLastChar(TEXT(':'), ColonIdx);
        if (ColonIdx == INDEX_NONE)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_CONTEXT,
                FString::Printf(TEXT("Context variable '%s': expected 'nodeGuid:pinName', got '%s'"),
                    *VarName, *Value));
            return false;
        }

        FString NodeGuidStr = Value.Left(ColonIdx);
        FString PinName = Value.Mid(ColonIdx + 1);

        FGuid NodeGuid;
        if (!FGuid::Parse(NodeGuidStr, NodeGuid))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_CONTEXT,
                FString::Printf(TEXT("Context variable '%s': invalid GUID '%s'"), *VarName, *NodeGuidStr));
            return false;
        }

        UEdGraphNode* Node = FBlueprintEditorUtils::GetNodeByGUID(BP, NodeGuid);
        if (!Node)
        {
            Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND,
                FString::Printf(TEXT("Context variable '%s': no node with GUID '%s'"), *VarName, *NodeGuidStr));
            return false;
        }

        // Find the output pin by name (case-insensitive)
        UEdGraphPin* ResolvedPin = nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                ResolvedPin = Pin;
                break;
            }
        }

        if (!ResolvedPin)
        {
            Ctx.SendError(ErrorCodes::ERR_PIN_NOT_FOUND,
                FString::Printf(TEXT("Context variable '%s': no output pin named '%s' on node '%s'"),
                    *VarName, *PinName, *NodeGuidStr));
            return false;
        }

        Compiler.InjectExternalVariable(VarName, ResolvedPin);
    }

    return true;
}

enum class EBpirPendingResponseKind
{
    None,
    Success,
    Error
};

struct FBpirPendingResponse
{
    EBpirPendingResponseKind Kind = EBpirPendingResponseKind::None;
    bool bRollback = false;
    FString ErrorCode;
    FString ErrorMessage;
    TSharedPtr<FJsonObject> Payload;
    BlueprintGraphSnapshot::FBlueprintGraphSnapshot Snapshot;
};

// Rolls back property-level mutations recorded in the active scoped transaction,
// then drops any node that bypassed the transaction buffer entirely (e.g.
// NewObject + Graph->AddNode without a paired Modify()).
//
// Rollback runs through UTransBuffer::Undo with bSuspendBroadcastPostUndoRedo
// set so the engine's pin serializer sees a valid CurrentTransaction during
// FTransaction::Apply (avoids the IsObjectTransacting ensure and the duplicate
// LinkedTo entries that follow), but the Blueprint editor's FEditorUndoClient
// is never notified (so MarkBlueprintAsStructurallyModified and the synchronous
// skeleton compile that hangs the HTTP transport never fire). See
// PinWrightTransactionUtils::ApplyAndCancelTransaction for the full trace.
//
// Caller invariant: must be invoked while `Transaction` is still in scope and
// outstanding. The snapshot pass runs after the Undo finishes since by then no
// transaction buffer state could resurrect the new nodes that bypassed Modify().
static bool RollbackBpirTransaction(FScopedTransaction& Transaction,
    const BlueprintGraphSnapshot::FBlueprintGraphSnapshot& Snapshot,
    FString* OutVerificationError = nullptr)
{
    PinWrightTransactionUtils::ApplyAndCancelTransaction(Transaction);
    BlueprintGraphSnapshot::RollbackToSnapshot(Snapshot);

    FString VerificationError;
    const bool bTopologyRestored =
        BlueprintGraphSnapshot::VerifyLinkTopology(Snapshot, VerificationError);
    if (OutVerificationError)
    {
        *OutVerificationError = MoveTemp(VerificationError);
    }
    return bTopologyRestored;
}

static void SendBpirPendingResponse(FHandlerContext& Ctx, const FBpirPendingResponse& Response)
{
    if (Response.Kind == EBpirPendingResponseKind::Success)
    {
        Ctx.SendSuccess(Response.Payload);
    }
    else if (Response.Kind == EBpirPendingResponseKind::Error)
    {
        Ctx.SendError(Response.ErrorCode, Response.ErrorMessage, Response.Payload);
    }
}

static void SetRollbackVerificationFailureResponse(
    FBpirPendingResponse& PendingResponse,
    const FString& Operation,
    const FString& VerificationError)
{
    const FString OriginalErrorCode = PendingResponse.ErrorCode;
    const FString OriginalErrorMessage = PendingResponse.ErrorMessage;
    if (!PendingResponse.Payload.IsValid())
    {
        PendingResponse.Payload = MakeShared<FJsonObject>();
    }
    PendingResponse.Payload->SetStringField(TEXT("originalErrorCode"), OriginalErrorCode);
    PendingResponse.Payload->SetStringField(TEXT("originalErrorMessage"), OriginalErrorMessage);
    PendingResponse.Payload->SetBoolField(TEXT("rollbackVerified"), false);
    PendingResponse.Payload->SetStringField(TEXT("rollbackError"), VerificationError);

    PendingResponse.Kind = EBpirPendingResponseKind::Error;
    PendingResponse.bRollback = false;
    PendingResponse.ErrorCode = ErrorCodes::ERR_INTERNAL_ERROR;
    PendingResponse.ErrorMessage = FString::Printf(
        TEXT("%s failed and rollback could not restore the original Blueprint link topology: %s"),
        *Operation,
        *VerificationError);
}

static FString BuildBlueprintCompileFailureMessage(
    const FString& Operation,
    const FBlueprintCompileDiagnostics& Diagnostics)
{
    if (Diagnostics.Errors.Num() > 0)
    {
        return FString::Printf(TEXT("%s failed Blueprint compile: %s"),
            *Operation,
            *FString::Join(Diagnostics.Errors, TEXT("; ")));
    }

    return FString::Printf(TEXT("%s failed Blueprint compile with status '%s'"),
        *Operation,
        *Diagnostics.Status);
}

static void SetBlueprintCompileFailureResponse(
    FBpirPendingResponse& PendingResponse,
    const TSharedPtr<FJsonObject>& Payload,
    const FString& Operation,
    const FBlueprintCompileDiagnostics& Diagnostics)
{
    PendingResponse.Kind = EBpirPendingResponseKind::Error;
    PendingResponse.bRollback = true;
    PendingResponse.ErrorCode = ErrorCodes::ERR_BLUEPRINT_COMPILE_FAILED;
    PendingResponse.ErrorMessage = BuildBlueprintCompileFailureMessage(Operation, Diagnostics);
    PendingResponse.Payload = Payload;
}

static void SetBlueprintIntegrityFailureResponse(
    FBpirPendingResponse& PendingResponse,
    const TSharedPtr<FJsonObject>& Out,
    const TArray<FBlueprintIntegrityFailure>& IntegrityFailures,
    const FString& Operation)
{
    Out->SetArrayField(TEXT("integrityFailures"),
        BuildBlueprintIntegrityFailuresJson(IntegrityFailures));
    Out->SetBoolField(TEXT("success"), false);
    PendingResponse.Kind = EBpirPendingResponseKind::Error;
    PendingResponse.bRollback = true;
    PendingResponse.ErrorCode = ErrorCodes::ERR_INTEGRITY_FAILURE;
    PendingResponse.ErrorMessage = FString::Printf(
        TEXT("%s produced a blueprint with %d integrity failure(s) — transaction rolled back."),
        *Operation,
        IntegrityFailures.Num());
    PendingResponse.Payload = Out;
}

// ---- blueprint.compile_bpir ----
REGISTER_RPC_HANDLER("blueprint.compile_bpir", "blueprint",
    "Compile BPIR (Blueprint IR) code into Blueprint graph nodes",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("code", "string", "BPIR code to compile"),
        RPC_PARAM_OPT("mode", "string", "Compile mode: 'append' (default) or 'extend'. append is upsert: existing entry nodes with matching signatures are deleted before re-creation, making retries idempotent. 'replace' is accepted as an alias for 'append' for backward compatibility. extend: appends new body to the terminal exec-output pin of the existing entry's chain; falls back to append when no existing entry is found; errors if the existing chain has forked (branch/sequence) exec flow. Scoped to entry override; other entry kinds fall back to append in extend mode."),
        RPC_PARAM_OPT("allowPreexistingErrors", "boolean", "Default false. When true, a valid placement is KEPT even if the whole-Blueprint compile still fails — provided THIS call introduced no new compile error versus a pre-placement baseline (i.e. every remaining error lives in a graph this call never touched). The response still reports compiled:false + status:Error + the full error list (never a false success), plus preexistingErrorsOnly:true. Lets you repair an already-broken Blueprint one graph at a time instead of having to fix every broken graph in a single call. Leaving it false preserves the atomic rollback-on-any-compile-failure behavior."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_BLUEPRINT_PATH,
            TEXT("blueprint.compile_bpir: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString Code;
    if (!Ctx.RequireString(TEXT("code"), Code)) return true;
    const FString Mode = Ctx.GetString(TEXT("mode"));
    // "append" / "replace" (default): upsert semantics — Phase 0 deletes matching existing
    // entry nodes before re-creation so RPC-timeout retries are idempotent. Maps to Replace
    // so callers get the documented upsert behavior. (Default mode without Phase 0 is reserved
    // for in-process tests that need atomic rollback + conflict-detection semantics.)
    // "extend": append to terminal exec pin of existing chain.
    EBpirCompileMode CompileMode = EBpirCompileMode::Replace;
    if (Mode.Equals(TEXT("extend"), ESearchCase::IgnoreCase))
    {
        CompileMode = EBpirCompileMode::Extend;
    }

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE,
            TEXT("blueprint.compile_bpir cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_NOT_FOUND,
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    const BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey =
        BlueprintReinstancingGuard::SurveyLiveInstances(BP);
    if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, BP, ReinstancingSurvey, TEXT("blueprint.compile_bpir")))
    {
        return true;
    }

    // Widget BPs need a pre-compile so PopulateBlueprintGeneratedVariables creates
    // FProperties for widget variables before the BPIR compiler tries to resolve $VarName
    if (Cast<UWidgetBlueprint>(BP))
    {
        CompileBlueprintWithDiagnostics(BP);
    }

    // Opt-in: keep a valid placement even when the whole-BP compile still fails, so
    // long as this call adds no NEW compile error versus the pre-placement baseline
    // (the remaining errors all predate the call, in graphs it never touched). The
    // baseline compile is only run when the caller opts in, so the common all-clean
    // path pays nothing extra and the default rollback-on-any-failure behavior is
    // byte-for-byte preserved. See E-compile-bpir-preexisting-errors-block-repair.
    const bool bAllowPreexistingErrors = Ctx.GetBool(TEXT("allowPreexistingErrors"), false);
    TArray<FString> BaselineCompileErrors;
    if (bAllowPreexistingErrors)
    {
        BaselineCompileErrors = CompileBlueprintWithDiagnostics(BP).Errors;
    }

    FBpirPendingResponse PendingResponse;
    PendingResponse.Snapshot = BlueprintGraphSnapshot::Capture(BP);
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.compile_bpir")));
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(Code, CompileMode);

        TArray<TSharedPtr<FJsonValue>> CreatedNodesArray;
        for (const FGuid& Guid : Result.CreatedNodeGUIDs)
        {
            CreatedNodesArray.Add(MakeShared<FJsonValueString>(Guid.ToString()));
        }

        TArray<TSharedPtr<FJsonValue>> ErrorsArray;
        for (const FCompileError& Err : Result.Errors)
        {
            TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
            ErrObj->SetNumberField(TEXT("line"), Err.Line);
            ErrObj->SetStringField(TEXT("message"), Err.Message);
            ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrObj));
        }

        TArray<TSharedPtr<FJsonValue>> WarningsArray;
        for (const FString& Warn : Result.Warnings)
        {
            WarningsArray.Add(MakeShared<FJsonValueString>(Warn));
        }

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("nodeCount"), Result.CreatedNodeGUIDs.Num());
        Out->SetArrayField(TEXT("createdNodes"), CreatedNodesArray);
        Out->SetArrayField(TEXT("errors"), ErrorsArray);
        Out->SetArrayField(TEXT("warnings"), WarningsArray);

        if (Result.OrphansRemoved > 0)
        {
            Out->SetNumberField(TEXT("orphansRemoved"), Result.OrphansRemoved);
        }

        if (Result.bSuccess)
        {
            const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(BP);
            AddCompileDiagnosticsToJson(Diagnostics, Out);
            BlueprintReinstancingGuard::AddSurveyToJson(ReinstancingSurvey, Out);
            for (const FString& Error : Diagnostics.Errors)
            {
                TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
                ErrObj->SetStringField(TEXT("message"), Error);
                ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrObj));
            }
            for (const FString& Warning : Diagnostics.Warnings)
            {
                WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
            }
            Out->SetArrayField(TEXT("errors"), ErrorsArray);
            Out->SetArrayField(TEXT("warnings"), WarningsArray);

            // Post-full-compile refresh of BPIR-emitted delegate nodes. HandleAnyChange during
            // Phase 2 ran against SkeletonGeneratedClass; the full compile above may have
            // shifted canonical custom-event state, leaving CreateDelegate.SelectedFunctionGuid
            // stale vs. the gate's GeneratedClass-scope canonical lookup. Narrow scope: only
            // nodes we just created in this transaction.
            RefreshBpirDelegateNodes(BP, Result.CreatedNodeGUIDs);

            // Pre-commit integrity gate — catch latent saved-state corruption that the
            // in-process compile can't detect. If found, roll the
            // transaction back so nothing from this BPIR compile persists in-memory
            // either. Without this, a later `asset.save` would still write an
            // un-loadable .uasset; cancelling here fails fast at the
            // emit site rather than waiting for the save-time gate.
            TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> IntegrityFailures;
            const bool bIntegrityOk = ValidateBlueprintGraphIntegrity(BP, IntegrityFailures);
            if (!bIntegrityOk)
            {
                SetBlueprintIntegrityFailureResponse(PendingResponse, Out, IntegrityFailures, TEXT("BPIR compile"));
            }
            else if (!Diagnostics.bCompiled)
            {
                // Whole-BP compile failed. Normally this rolls the placement back. In
                // allowPreexistingErrors mode, keep an otherwise-valid placement when it
                // introduced no NEW error vs. the pre-placement baseline — the remaining
                // errors predate this call (untouched graphs), so discarding the good
                // edit would only block incremental repair. compiled:false + status +
                // the full error list stay set (from above), so this is never a false
                // success — the caller is told the BP is not yet compilable.
                if (bAllowPreexistingErrors
                    && !HasNewCompileErrorsBeyondBaseline(BaselineCompileErrors, Diagnostics.Errors))
                {
                    Out->SetBoolField(TEXT("success"), true);
                    Out->SetBoolField(TEXT("preexistingErrorsOnly"), true);
                    PendingResponse.Kind = EBpirPendingResponseKind::Success;
                    PendingResponse.Payload = Out;
                }
                else
                {
                    // Roll back: either the flag is off, or it is on but this placement
                    // added a NEW error. Keep the compile and reinstancing diagnostics in
                    // the error payload so failure has the same observability as success.
                    Out->SetBoolField(TEXT("success"), false);
                    SetBlueprintCompileFailureResponse(
                        PendingResponse, Out, TEXT("BPIR compile"), Diagnostics);
                }
            }
            else
            {
                Out->SetBoolField(TEXT("success"), true);
                PendingResponse.Kind = EBpirPendingResponseKind::Success;
                PendingResponse.Payload = Out;
            }
        }
        else
        {
            FString ErrorMessage = TEXT("BPIR compilation failed");
            if (Result.Errors.Num() > 0)
            {
                TArray<FString> ErrorStrings;
                for (const FCompileError& Err : Result.Errors)
                {
                    ErrorStrings.Add(FString::Printf(TEXT("Line %d: %s"), Err.Line, *Err.Message));
                }
                ErrorMessage = FString::Join(ErrorStrings, TEXT("; "));
            }
            Out->SetBoolField(TEXT("success"), false);
            PendingResponse.Kind = EBpirPendingResponseKind::Error;
            PendingResponse.bRollback = true;
            PendingResponse.ErrorCode = ErrorCodes::ERR_COMPILE_FAILED;
            PendingResponse.ErrorMessage = ErrorMessage;
        }

        if (PendingResponse.bRollback)
        {
            RollbackBpirTransaction(Transaction, PendingResponse.Snapshot);
        }
    }
    SendBpirPendingResponse(Ctx, PendingResponse);
    return true;
}

// ---- blueprint.insert_bpir_at_node ----
REGISTER_RPC_HANDLER("blueprint.insert_bpir_at_node", "blueprint",
    "Insert BPIR code after a selected node in the graph",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node GUID or name to insert after"),
        RPC_PARAM_REQ("code", "string", "BPIR body code (no entry wrapper needed)"),
        RPC_PARAM_OPT("execPin", "string", "Exec output pin name to insert after (default: first exec output)"),
        RPC_PARAM_OPT("context", "object", "Map of variable names to 'nodeGuid:pinName'. Variables become $VarName in BPIR code."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_BLUEPRINT_PATH,
            TEXT("blueprint.insert_bpir_at_node: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString NodeId, Code;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;
    if (!Ctx.RequireString(TEXT("code"), Code)) return true;
    FString ExecPinName = Ctx.GetString(TEXT("execPin"));

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE,
            TEXT("blueprint.insert_bpir_at_node cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_NOT_FOUND,
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    const BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey =
        BlueprintReinstancingGuard::SurveyLiveInstances(BP);
    if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, BP, ReinstancingSurvey, TEXT("blueprint.insert_bpir_at_node")))
    {
        return true;
    }

    // Resolve node by GUID first, then fall back to name matching
    UEdGraphNode* TargetNode = nullptr;

    FGuid NodeGuid;
    if (FGuid::Parse(NodeId, NodeGuid))
    {
        TargetNode = FBlueprintEditorUtils::GetNodeByGUID(BP, NodeGuid);
    }

    if (!TargetNode)
    {
        TArray<UEdGraph*> AllGraphs;
        BP->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                if (Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Equals(
                        NodeId, ESearchCase::IgnoreCase))
                {
                    TargetNode = Node;
                    break;
                }
            }
            if (TargetNode) break;
        }
    }

    if (!TargetNode)
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND,
            FString::Printf(TEXT("No node found with id or name '%s'"), *NodeId));
        return true;
    }

    // Widget BPs need a pre-compile so PopulateBlueprintGeneratedVariables creates
    // FProperties for widget variables before the BPIR compiler tries to resolve $VarName
    if (Cast<UWidgetBlueprint>(BP))
    {
        CompileBlueprintWithDiagnostics(BP);
    }

    FBpirCompiler Compiler(BP);
    if (!ResolveContextInjections(Ctx, BP, Compiler))
    {
        return true;
    }
    FBpirPendingResponse PendingResponse;
    PendingResponse.Snapshot = BlueprintGraphSnapshot::Capture(BP);
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.insert_bpir_at_node")));
        FCompileResult Result = Compiler.InsertCodeAfterNode(TargetNode, Code, ExecPinName);

        TArray<TSharedPtr<FJsonValue>> ErrorsArray;
        for (const FCompileError& Err : Result.Errors)
        {
            TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
            ErrObj->SetNumberField(TEXT("line"), Err.Line);
            ErrObj->SetStringField(TEXT("message"), Err.Message);
            ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrObj));
        }

        TArray<TSharedPtr<FJsonValue>> CreatedNodesArray;
        for (const FGuid& Guid : Result.CreatedNodeGUIDs)
        {
            CreatedNodesArray.Add(MakeShared<FJsonValueString>(Guid.ToString()));
        }

        TArray<TSharedPtr<FJsonValue>> WarningsArray;
        for (const FString& Warn : Result.Warnings)
        {
            WarningsArray.Add(MakeShared<FJsonValueString>(Warn));
        }

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("nodeCount"), Result.CreatedNodeGUIDs.Num());
        Out->SetArrayField(TEXT("createdNodes"), CreatedNodesArray);
        Out->SetArrayField(TEXT("errors"), ErrorsArray);
        Out->SetArrayField(TEXT("warnings"), WarningsArray);

        if (Result.bSuccess)
        {
            const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(BP);
            AddCompileDiagnosticsToJson(Diagnostics, Out);
            BlueprintReinstancingGuard::AddSurveyToJson(ReinstancingSurvey, Out);
            for (const FString& Error : Diagnostics.Errors)
            {
                TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
                ErrObj->SetStringField(TEXT("message"), Error);
                ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrObj));
            }
            for (const FString& Warning : Diagnostics.Warnings)
            {
                WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
            }
            Out->SetArrayField(TEXT("errors"), ErrorsArray);
            Out->SetArrayField(TEXT("warnings"), WarningsArray);

            // Post-full-compile refresh — see compile_bpir handler above for rationale.
            RefreshBpirDelegateNodes(BP, Result.CreatedNodeGUIDs);

            TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> IntegrityFailures;
            if (!ValidateBlueprintGraphIntegrity(BP, IntegrityFailures))
            {
                SetBlueprintIntegrityFailureResponse(PendingResponse, Out, IntegrityFailures, TEXT("BPIR insert"));
            }
            else if (!Diagnostics.bCompiled)
            {
                Out->SetBoolField(TEXT("success"), false);
                SetBlueprintCompileFailureResponse(
                    PendingResponse, Out, TEXT("BPIR insert"), Diagnostics);
            }
            else
            {
                Out->SetBoolField(TEXT("success"), true);
                PendingResponse.Kind = EBpirPendingResponseKind::Success;
                PendingResponse.Payload = Out;
            }
        }
        else
        {
            FString ErrorMessage = TEXT("BPIR insertion failed");
            if (Result.Errors.Num() > 0)
            {
                TArray<FString> ErrorStrings;
                for (const FCompileError& Err : Result.Errors)
                {
                    ErrorStrings.Add(FString::Printf(TEXT("Line %d: %s"), Err.Line, *Err.Message));
                }
                ErrorMessage = FString::Join(ErrorStrings, TEXT("; "));
            }
            Out->SetBoolField(TEXT("success"), false);
            PendingResponse.Kind = EBpirPendingResponseKind::Error;
            PendingResponse.bRollback = true;
            PendingResponse.ErrorCode = ErrorCodes::ERR_COMPILE_FAILED;
            PendingResponse.ErrorMessage = ErrorMessage;
        }

        if (PendingResponse.bRollback)
        {
            FString RollbackVerificationError;
            if (!RollbackBpirTransaction(
                    Transaction,
                    PendingResponse.Snapshot,
                    &RollbackVerificationError))
            {
                SetRollbackVerificationFailureResponse(
                    PendingResponse,
                    TEXT("blueprint.insert_bpir_at_node"),
                    RollbackVerificationError);
            }
        }
    }
    SendBpirPendingResponse(Ctx, PendingResponse);
    return true;
}

// ---- blueprint.insert_bpir_before_node ----
REGISTER_RPC_HANDLER("blueprint.insert_bpir_before_node", "blueprint",
    "Insert BPIR code before a selected node in the graph",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node GUID or name to insert before"),
        RPC_PARAM_REQ("code", "string", "BPIR body code (no entry wrapper needed)"),
        RPC_PARAM_OPT("context", "object", "Map of variable names to 'nodeGuid:pinName'. Variables become $VarName in BPIR code."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_BLUEPRINT_PATH,
            TEXT("blueprint.insert_bpir_before_node: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString NodeId, Code;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;
    if (!Ctx.RequireString(TEXT("code"), Code)) return true;

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE,
            TEXT("blueprint.insert_bpir_before_node cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_NOT_FOUND,
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    const BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey =
        BlueprintReinstancingGuard::SurveyLiveInstances(BP);
    if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, BP, ReinstancingSurvey, TEXT("blueprint.insert_bpir_before_node")))
    {
        return true;
    }

    // Resolve node by GUID first, then fall back to name matching
    UEdGraphNode* TargetNode = nullptr;

    FGuid NodeGuid;
    if (FGuid::Parse(NodeId, NodeGuid))
    {
        TargetNode = FBlueprintEditorUtils::GetNodeByGUID(BP, NodeGuid);
    }

    if (!TargetNode)
    {
        TArray<UEdGraph*> AllGraphs;
        BP->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                if (Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Equals(
                        NodeId, ESearchCase::IgnoreCase))
                {
                    TargetNode = Node;
                    break;
                }
            }
            if (TargetNode) break;
        }
    }

    if (!TargetNode)
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND,
            FString::Printf(TEXT("No node found with id or name '%s'"), *NodeId));
        return true;
    }

    // Widget BPs need a pre-compile so PopulateBlueprintGeneratedVariables creates
    // FProperties for widget variables before the BPIR compiler tries to resolve $VarName
    if (Cast<UWidgetBlueprint>(BP))
    {
        CompileBlueprintWithDiagnostics(BP);
    }

    FBpirCompiler Compiler(BP);
    if (!ResolveContextInjections(Ctx, BP, Compiler))
    {
        return true;
    }
    FBpirPendingResponse PendingResponse;
    PendingResponse.Snapshot = BlueprintGraphSnapshot::Capture(BP);
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.insert_bpir_before_node")));
        FCompileResult Result = Compiler.InsertCodeBeforeNode(TargetNode, Code);

        TArray<TSharedPtr<FJsonValue>> ErrorsArray;
        for (const FCompileError& Err : Result.Errors)
        {
            TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
            ErrObj->SetNumberField(TEXT("line"), Err.Line);
            ErrObj->SetStringField(TEXT("message"), Err.Message);
            ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrObj));
        }

        TArray<TSharedPtr<FJsonValue>> CreatedNodesArray;
        for (const FGuid& Guid : Result.CreatedNodeGUIDs)
        {
            CreatedNodesArray.Add(MakeShared<FJsonValueString>(Guid.ToString()));
        }

        TArray<TSharedPtr<FJsonValue>> WarningsArray;
        for (const FString& Warn : Result.Warnings)
        {
            WarningsArray.Add(MakeShared<FJsonValueString>(Warn));
        }

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("nodeCount"), Result.CreatedNodeGUIDs.Num());
        Out->SetArrayField(TEXT("createdNodes"), CreatedNodesArray);
        Out->SetArrayField(TEXT("errors"), ErrorsArray);
        Out->SetArrayField(TEXT("warnings"), WarningsArray);

        if (Result.bSuccess)
        {
            const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(BP);
            AddCompileDiagnosticsToJson(Diagnostics, Out);
            BlueprintReinstancingGuard::AddSurveyToJson(ReinstancingSurvey, Out);
            for (const FString& Error : Diagnostics.Errors)
            {
                TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
                ErrObj->SetStringField(TEXT("message"), Error);
                ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrObj));
            }
            for (const FString& Warning : Diagnostics.Warnings)
            {
                WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
            }
            Out->SetArrayField(TEXT("errors"), ErrorsArray);
            Out->SetArrayField(TEXT("warnings"), WarningsArray);

            // Post-full-compile refresh — see compile_bpir handler above for rationale.
            RefreshBpirDelegateNodes(BP, Result.CreatedNodeGUIDs);

            TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> IntegrityFailures;
            if (!ValidateBlueprintGraphIntegrity(BP, IntegrityFailures))
            {
                SetBlueprintIntegrityFailureResponse(PendingResponse, Out, IntegrityFailures, TEXT("BPIR insert"));
            }
            else if (!Diagnostics.bCompiled)
            {
                Out->SetBoolField(TEXT("success"), false);
                SetBlueprintCompileFailureResponse(
                    PendingResponse, Out, TEXT("BPIR insert"), Diagnostics);
            }
            else
            {
                Out->SetBoolField(TEXT("success"), true);
                PendingResponse.Kind = EBpirPendingResponseKind::Success;
                PendingResponse.Payload = Out;
            }
        }
        else
        {
            FString ErrorMessage = TEXT("BPIR insertion failed");
            if (Result.Errors.Num() > 0)
            {
                TArray<FString> ErrorStrings;
                for (const FCompileError& Err : Result.Errors)
                {
                    ErrorStrings.Add(FString::Printf(TEXT("Line %d: %s"), Err.Line, *Err.Message));
                }
                ErrorMessage = FString::Join(ErrorStrings, TEXT("; "));
            }
            Out->SetBoolField(TEXT("success"), false);
            PendingResponse.Kind = EBpirPendingResponseKind::Error;
            PendingResponse.bRollback = true;
            PendingResponse.ErrorCode = ErrorCodes::ERR_COMPILE_FAILED;
            PendingResponse.ErrorMessage = ErrorMessage;
        }

        if (PendingResponse.bRollback)
        {
            RollbackBpirTransaction(Transaction, PendingResponse.Snapshot);
        }
    }
    SendBpirPendingResponse(Ctx, PendingResponse);
    return true;
}

// ---- blueprint.undo_last_bpir ----
REGISTER_RPC_HANDLER("blueprint.undo_last_bpir", "blueprint",
    "Undo the last BPIR compilation by deleting created nodes",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path"))
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_BLUEPRINT_PATH,
            TEXT("blueprint.undo_last_bpir: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_NOT_FOUND,
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    // Refuse rollback when the last compile ran Phase 0 sweeps. Phase 0 deletes pre-existing
    // entry nodes (and their exec-reachable subgraphs) before re-creating them — un-creating
    // the new nodes here cannot restore the swept ones, so a "successful" undo would leave the
    // blueprint more broken than the compile + no undo. Caller falls back to git / asset.revert.
    // See B-undo-last-bpir-doesnt-restore-phase0-sweeps.
    if (FBpirCompiler::DidLastCompileRunPhase0())
    {
        Ctx.SendError(ErrorCodes::ERR_UNDO_NOT_REVERSIBLE,
            TEXT("undo_last_bpir cannot reverse Phase 0 sweeps from the previous compile. "
                 "The compile ran in append/replace mode and deleted pre-existing entry subgraphs "
                 "that aren't snapshotted for restoration. Use git revert or asset.revert instead."));
        return true;
    }

    TArray<FGuid> Nodes = FBpirCompiler::PopLastCreatedNodes();
    if (Nodes.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_NOTHING_TO_UNDO, TEXT("Nothing to undo: no previous BPIR compile on record"));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.undo_last_bpir")));
    FBpirCompiler::DeleteNodesByGUIDs(BP, Nodes);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("success"), true);
    Out->SetNumberField(TEXT("deletedCount"), Nodes.Num());
    Ctx.SendSuccess(Out);
    return true;
}

// ---- blueprint.get_node_connections ----
REGISTER_RPC_HANDLER("blueprint.get_node_connections", "blueprint",
    "Get pin connections for a node (upstream and downstream)",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("nodeId", "string", "Node GUID or name"),
        RPC_PARAM_OPT("includeDataPins", "boolean", "Include data pin connections (default true)")
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_BLUEPRINT_PATH,
            TEXT("blueprint.get_node_connections: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_NOT_FOUND,
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    // Resolve node by GUID first, then fall back to name matching
    UEdGraphNode* TargetNode = nullptr;

    FGuid NodeGuid;
    if (FGuid::Parse(NodeId, NodeGuid))
    {
        TargetNode = FBlueprintEditorUtils::GetNodeByGUID(BP, NodeGuid);
    }

    if (!TargetNode)
    {
        TArray<UEdGraph*> AllGraphs;
        BP->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                if (Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Equals(
                        NodeId, ESearchCase::IgnoreCase))
                {
                    TargetNode = Node;
                    break;
                }
            }
            if (TargetNode) break;
        }
    }

    if (!TargetNode)
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND,
            FString::Printf(TEXT("No node found with id or name '%s'"), *NodeId));
        return true;
    }

    const bool bIncludeDataPins = Ctx.GetBool(TEXT("includeDataPins"), true);

    // Build exec and (optionally) data connection arrays
    TArray<TSharedPtr<FJsonValue>> ExecInputs;
    TArray<TSharedPtr<FJsonValue>> ExecOutputs;
    TArray<TSharedPtr<FJsonValue>> DataInputs;
    TArray<TSharedPtr<FJsonValue>> DataOutputs;

    for (UEdGraphPin* Pin : TargetNode->Pins)
    {
        if (!Pin) continue;

        const bool bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);

        // Skip data pins if not requested
        if (!bIsExec && !bIncludeDataPins)
        {
            continue;
        }

        // Determine which target arrays to use
        TArray<TSharedPtr<FJsonValue>>& InputArray = bIsExec ? ExecInputs : DataInputs;
        TArray<TSharedPtr<FJsonValue>>& OutputArray = bIsExec ? ExecOutputs : DataOutputs;

        if (Pin->Direction == EGPD_Input)
        {
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin) continue;
                UEdGraphNode* ConnectedNode = LinkedPin->GetOwningNode();
                if (!ConnectedNode) continue;

                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
                Entry->SetStringField(TEXT("connectedNodeId"), ConnectedNode->NodeGuid.ToString());
                Entry->SetStringField(TEXT("connectedNodeTitle"),
                    ConnectedNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                Entry->SetStringField(TEXT("connectedPinName"), LinkedPin->PinName.ToString());
                Entry->SetStringField(TEXT("pinCategory"), Pin->PinType.PinCategory.ToString());
                InputArray.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        else if (Pin->Direction == EGPD_Output)
        {
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin) continue;
                UEdGraphNode* ConnectedNode = LinkedPin->GetOwningNode();
                if (!ConnectedNode) continue;

                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
                Entry->SetStringField(TEXT("connectedNodeId"), ConnectedNode->NodeGuid.ToString());
                Entry->SetStringField(TEXT("connectedNodeTitle"),
                    ConnectedNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                Entry->SetStringField(TEXT("connectedPinName"), LinkedPin->PinName.ToString());
                Entry->SetStringField(TEXT("pinCategory"), Pin->PinType.PinCategory.ToString());
                OutputArray.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("nodeId"), TargetNode->NodeGuid.ToString());
    Out->SetStringField(TEXT("nodeTitle"),
        TargetNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    Out->SetArrayField(TEXT("execInputs"), ExecInputs);
    Out->SetArrayField(TEXT("execOutputs"), ExecOutputs);
    if (bIncludeDataPins)
    {
        Out->SetArrayField(TEXT("dataInputs"), DataInputs);
        Out->SetArrayField(TEXT("dataOutputs"), DataOutputs);
    }
    Ctx.SendSuccess(Out);
    return true;
}
