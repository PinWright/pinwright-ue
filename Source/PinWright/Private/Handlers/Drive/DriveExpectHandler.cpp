// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveWebHandlers.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"

// drive.expect — evaluate a single condition against a surface RIGHT NOW (no settle).
// Reports whether the condition is met plus the actual/expected/detail strings.
REGISTER_RPC_HANDLER("drive.expect", "drive",
    "Evaluate a condition against a live surface immediately (synchronous, no settle/wait).",
    RPC_PARAMS(
        RPC_PARAM_DEF("surface", "string", "Surface to evaluate against: game | editor_chrome | web | auto (default auto -> game).", "auto"),
        RPC_PARAM_OPT("instance_name", "string", "Live UMG root selector: substring-matched against the backing widget name."),
        RPC_PARAM_OPT("instanceName", "string", "Alias for instance_name."),
        RPC_PARAM_OPT("root_index", "integer", "Live UMG root selector: the Nth root (0-based). Takes precedence over instance_name."),
        RPC_PARAM_OPT("rootIndex", "integer", "Alias for root_index."),
        DRIVE_WINDOW_SELECTOR_PARAMS,
        RPC_PARAM_REQ("condition", "object", "Condition to evaluate (type + target/expected_text/expected_count/count_op/expected_bounds/severity).")
    ))
{
    FDriveCondition Condition;
    if (!FDriveJson::ParseCondition(Ctx.GetObject(TEXT("condition")), Condition))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
            TEXT("The 'condition' object is missing or has an unrecognized 'type'."));
        return true;
    }

    const EDriveSurface Surface = FDriveHandlerCommon::ResolveSurface(Ctx);

    // Web is fully asynchronous (the condition is evaluated against a CEF DOM query), so it
    // branches to the dedicated async path before the synchronous game/editor flow. ExpectWeb
    // re-parses the condition itself; the parse above still guards a malformed condition for
    // every surface.
    if (Surface == EDriveSurface::Web)
    {
        FDriveWebHandlers::ExpectWeb(Ctx);
        return true;
    }

    const FDriveRootSelector Selector = FDriveHandlerCommon::ParseRootSelector(Ctx);
    // Editor chrome can be evaluated against a named window, not just the active one; the game
    // surface ignores this selector.
    const FDriveWindowSelector WindowSelector = FDriveHandlerCommon::ParseWindowSelector(Ctx);

    TArray<FDriveElement> Elements;
    FString RootName;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FDriveHandlerCommon::GetElementsForSurface(Surface, Selector, Elements, RootName, ErrorCode, ErrorMessage, WindowSelector))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    // Journal conditions read the live tail; widget/text/count/geometry conditions
    // only read Elements, so the delta stays empty for those.
    FDriveJournalDelta Journal;
    const bool bJournalCondition =
        Condition.Type == EDriveConditionType::JournalEvent ||
        Condition.Type == EDriveConditionType::JournalSeverity;
    if (bJournalCondition)
    {
        FDriveHandlerCommon::GetJournalDelta(0, Journal);
    }

    const FDriveConditionResult Result = FDriveConditionEval::Evaluate(Condition, Elements, Journal);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("met"), Result.bMet);
    Out->SetStringField(TEXT("actual"), Result.Actual);
    Out->SetStringField(TEXT("expected"), Result.Expected);
    Out->SetStringField(TEXT("detail"), Result.Detail);
    Out->SetStringField(TEXT("root_name"), RootName);
    Ctx.SendSuccess(Out);
    return true;
}
