// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveWebHandlers.h"

// drive.observe — capture one observation of a surface: its addressable elements,
// an optional Set-of-Mark screenshot (the default observation), and optional journal.
REGISTER_RPC_HANDLER("drive.observe", "drive",
    "Observe a live UI surface: its elements plus an optional Set-of-Mark screenshot and journal delta.",
    RPC_PARAMS(
        RPC_PARAM_DEF("surface", "string", "Surface to observe: game | editor_chrome | web | auto (default auto -> game).", "auto"),
        RPC_PARAM_OPT("instance_name", "string", "Live UMG root selector: substring-matched against the backing widget name."),
        RPC_PARAM_OPT("instanceName", "string", "Alias for instance_name."),
        RPC_PARAM_OPT("root_index", "integer", "Live UMG root selector: the Nth root (0-based). Takes precedence over instance_name."),
        RPC_PARAM_OPT("rootIndex", "integer", "Alias for root_index."),
        DRIVE_WINDOW_SELECTOR_PARAMS,
        RPC_PARAM_DEF("screenshot", "boolean", "Attach a Set-of-Mark annotated screenshot (default true). See screenshot_mode for inline-base64 vs file delivery.", "true"),
        RPC_PARAM_DEF("screenshot_mode", "string", "How the screenshot is delivered: 'inline' (default) embeds it as base64 under screenshot.base64; 'file' instead writes the PNG to Saved/Screenshots/Drive and returns its path under screenshot.path (with width/height/marks, no base64), keeping the response small and the image directly Read-able.", "inline"),
        RPC_PARAM_DEF("mark_cap", "number", "Max interactables given a visible mark; the rest are reported omitted (default 50).", "50"),
        RPC_PARAM_DEF("interactables_only", "boolean", "Return only interactable elements (buttons/inputs), dropping static text/labels (default false).", "false"),
        RPC_PARAM_DEF("max_elements", "number", "Cap the elements array at this many; 0 = unlimited. When the cap drops elements, omitted_count reports how many (default 0).", "0"),
        RPC_PARAM_DEF("max_bytes", "number", "Cap the total serialized size of the elements array at approximately this many bytes; 0 = unlimited (default). Each element's handle is the full widget-tree path, so interactables_only/max_elements bound element COUNT but not payload BYTES; set max_bytes to keep the element list from spilling to a file. max_bytes caps ONLY the element list, so a scan/discovery observe must ALSO drop the default inline screenshot (screenshot:false, or screenshot_mode:file) to stay inline, because a single inline base64 screenshot alone exceeds the inline budget regardless of max_bytes. At least one element is always returned; when the cap drops elements, omitted_count reports how many. For a pure presence check ('is control X on this surface?') prefer drive.expect widget_present, which returns a small fixed shape and never spills.", "0"),
        RPC_PARAM_DEF("include_journal", "boolean", "Attach a journal delta since journal_since (default false).", "false"),
        RPC_PARAM_DEF("journal_since", "number", "Cursor to read journal from when include_journal is set (default 0).", "0")
    ))
{
    const EDriveSurface Surface = FDriveHandlerCommon::ResolveSurface(Ctx);

    // Web is fully asynchronous (every observation is a CEF console round-trip), so it
    // branches to the dedicated async path before the synchronous game/editor flow.
    if (Surface == EDriveSurface::Web)
    {
        FDriveWebHandlers::ObserveWeb(Ctx);
        return true;
    }

    const FDriveRootSelector Selector = FDriveHandlerCommon::ParseRootSelector(Ctx);
    // Editor chrome is addressed by window title / index so an agent can observe a named
    // window, not just the active one; the game surface ignores this selector.
    const FDriveWindowSelector WindowSelector = FDriveHandlerCommon::ParseWindowSelector(Ctx);

    const bool bScreenshot = Ctx.GetBool(TEXT("screenshot"), true);
    const bool bScreenshotToFile = FDriveHandlerCommon::ParseScreenshotToFile(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bInteractablesOnly = Ctx.GetBool(TEXT("interactables_only"), false);
    const int32 MaxElements = Ctx.GetInt(TEXT("max_elements"), 0);
    const int32 MaxBytes = Ctx.GetInt(TEXT("max_bytes"), 0);
    const bool bIncludeJournal = Ctx.GetBool(TEXT("include_journal"), false);
    const int32 SinceRaw = Ctx.GetInt(TEXT("journal_since"), 0);
    const uint64 JournalSince = SinceRaw > 0 ? static_cast<uint64>(SinceRaw) : 0;

    FDriveObservation Observation;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FDriveHandlerCommon::BuildObservation(
            Surface, Selector, bScreenshot, MarkCap, bIncludeJournal, JournalSince,
            Observation, ErrorCode, ErrorMessage, WindowSelector, bInteractablesOnly, MaxElements,
            bScreenshotToFile, MaxBytes))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    Ctx.SendSuccess(FDriveJson::WriteObservation(Observation));
    return true;
}
