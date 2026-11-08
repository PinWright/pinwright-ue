// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveJson.h"

// drive.events_since — drain the live journal tail from a cursor: the newly emitted
// events plus changed variables, and the new cursor to poll from next.
REGISTER_RPC_HANDLER("drive.events_since", "drive",
    "Read the journal delta (events + changed variables) since a cursor; empty delta when no PIE/session is active.",
    RPC_PARAMS(
        RPC_PARAM_DEF("since", "number", "Cursor to read from; pass back the previous response's cursor to poll incrementally (default 0).", "0")
    ))
{
    const int32 SinceRaw = Ctx.GetInt(TEXT("since"), 0);
    const uint64 Since = SinceRaw > 0 ? static_cast<uint64>(SinceRaw) : 0;

    // No live tail (no PIE/session) yields an empty delta with cursor 0 — not an error.
    FDriveJournalDelta Delta;
    FDriveHandlerCommon::GetJournalDelta(Since, Delta);

    Ctx.SendSuccess(FDriveJson::WriteJournalDelta(Delta));
    return true;
}
