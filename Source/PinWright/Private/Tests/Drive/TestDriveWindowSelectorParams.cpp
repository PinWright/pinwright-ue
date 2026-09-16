// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-drive-window-selector-param-unreachable.
// Every editor-chrome drive.* verb (observe, expect, and the six action verbs
// click/hover/scroll/type/key/drag) reads a window selector from window_title/title
// and window_index/index via FDriveHandlerCommon::ParseWindowSelector — but the verbs
// did NOT declare those keys in their RPC_PARAMS. The dispatcher builds its param
// allowlist purely from the declared RPC_PARAMS (RpcDispatcher.cpp ValidateHandlerParams),
// so an incoming window_title/window_index was hard-rejected with UNKNOWN_PARAMS before
// the handler body ran. The documented list_windows -> window_index/window_title targeting
// was therefore unreachable dead code. The fix declares the four optional selector keys on
// drive.observe, drive.expect, and the shared DRIVE_COMMON_ACTION_PARAMS macro (the action
// verbs); no handler-body change is needed since ParseWindowSelector already reads them.
//
// Two layers of coverage, mirroring TestActorNameParamAlias / TestAssetPathParamAlias:
//  - Static registration check: every editor-chrome drive verb's production FParamSpec set
//    declares window_title/title (string) and window_index/index (integer) as optional.
//    Fails if ANY of the three declaration edits (observe, expect, or the action macro)
//    is reverted.
//  - End-to-end dispatch: route {surface:editor_chrome, window_title/window_index} through
//    the real dispatcher and assert it is NOT rejected UNKNOWN_PARAMS — i.e. the selector
//    passed ValidateHandlerParams and reached the body (which reports a domain window
//    outcome instead). This reproduces the ticket's verbatim drive.observe repro.
// Counterfactual: reverting the RPC_PARAMS additions makes the dispatcher reject the
// selector with UNKNOWN_PARAMS and the static check fails (the specs are absent).
//
// No content fixture is loaded: the payloads are built in-code and the selectors are a
// guaranteed-miss title (fresh GUID) / out-of-range index, so the outcome depends only on
// the param allowlist, never on which editor windows happen to be open.
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"

namespace DriveWindowSelectorParamsTestLocal
{
    // Every drive.* verb whose body reads FDriveHandlerCommon::ParseWindowSelector: the two
    // synchronous verbs plus the six action verbs that share DRIVE_COMMON_ACTION_PARAMS.
    // (drive.wait / drive.events_since / drive.list_windows do not read the selector.)
    const TArray<FString>& EditorChromeDriveVerbs()
    {
        static const TArray<FString> Verbs = {
            TEXT("drive.observe"),
            TEXT("drive.expect"),
            TEXT("drive.click"),
            TEXT("drive.hover"),
            TEXT("drive.scroll"),
            TEXT("drive.type"),
            TEXT("drive.key"),
            TEXT("drive.drag")
        };
        return Verbs;
    }
}

// 1. Static: each editor-chrome drive verb registers the window selector keys the handler
//    body reads (window_title/title as string, window_index/index as integer), all optional.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveVerbsDeclareWindowSelectorParamsTest,
    "PinWright.drive.aliases.VerbsDeclareWindowSelectorParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveVerbsDeclareWindowSelectorParamsTest::RunTest(const FString& Parameters)
{
    using ParamSpecTestHelpers::FindParamSpec;

    struct FExpectedParam { const TCHAR* Name; const TCHAR* Type; };
    const FExpectedParam SelectorParams[] = {
        { TEXT("window_title"), TEXT("string") },
        { TEXT("title"),        TEXT("string") },
        { TEXT("window_index"), TEXT("integer") },
        { TEXT("index"),        TEXT("integer") }
    };

    for (const FString& Verb : DriveWindowSelectorParamsTestLocal::EditorChromeDriveVerbs())
    {
        for (const FExpectedParam& Expected : SelectorParams)
        {
            const FParamSpec* Spec = FindParamSpec(Verb, Expected.Name);
            if (!TestNotNull(*FString::Printf(TEXT("%s declares the '%s' window-selector param"),
                    *Verb, Expected.Name), Spec))
            {
                continue;
            }
            TestFalse(*FString::Printf(TEXT("%s '%s' is optional"), *Verb, Expected.Name),
                Spec->bRequired);
            TestEqual(*FString::Printf(TEXT("%s '%s' type"), *Verb, Expected.Name),
                Spec->Type, FString(Expected.Type));
        }
    }
    return true;
}

// 2. End-to-end: drive.observe accepts the editor-chrome window selector at the wire level —
//    the selector passes ValidateHandlerParams so the request is never rejected UNKNOWN_PARAMS
//    (it reaches the body, which reports a domain window outcome). Reproduces the ticket repro.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveAcceptsWindowSelectorOnWireTest,
    "PinWright.drive.aliases.ObserveAcceptsWindowSelectorOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveObserveAcceptsWindowSelectorOnWireTest::RunTest(const FString& Parameters)
{
    using DispatcherTestHelpers::MakeDispatcher;
    using DispatcherTestHelpers::Dispatch;

    // Resolving no window (guaranteed-miss title / out-of-range index) short-circuits before
    // any screenshot, so the editor-chrome walk emits only ambient no-window noise; suppress it.
    bSuppressLogErrors = true;
    bSuppressLogWarnings = true;

    // A fresh GUID can never be a substring of any real window title, so the title case is a
    // deterministic miss regardless of host/editor window state.
    const FString MissTitle = FString::Printf(TEXT("PW_NoSuchWindow_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Case 1 — window_title (ticket repro #3).
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
        Params->SetStringField(TEXT("window_title"), MissTitle);
        Params->SetBoolField(TEXT("screenshot"), false);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("drive.observe"),
            TEXT("req-observe-window-title"), Params, bSuccess, ErrorCode);

        TestNotEqual(TEXT("drive.observe does not reject window_title as UNKNOWN_PARAMS"),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    }

    // Case 2 — window_index (ticket repro #2). Out-of-range so it can never resolve a window
    // (and never screenshot), independent of the live editor window count.
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
        Params->SetNumberField(TEXT("window_index"), 1000000);
        Params->SetBoolField(TEXT("screenshot"), false);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("drive.observe"),
            TEXT("req-observe-window-index"), Params, bSuccess, ErrorCode);

        TestNotEqual(TEXT("drive.observe does not reject window_index as UNKNOWN_PARAMS"),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    }

    return true;
}
