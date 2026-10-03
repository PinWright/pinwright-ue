// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-drive-browser-index-param-unreachable.
// Every surface=web drive verb (observe, expect, wait_for and the six action verbs) reads
// browser_index in its FDriveWebHandlers body, but none declared it in RPC_PARAMS, so the
// dispatcher's allowlist (RpcDispatcher.cpp ValidateHandlerParams) refused it UNKNOWN_PARAMS
// before the body ran: only browser 0 was reachable. The fix declares it once
// (DRIVE_BROWSER_SELECTOR_PARAM) at the four drive declaration sites.
//
// The wire test routes each verb through the REAL dispatcher (not a direct handler call, which
// is how the web live tests missed this) with an out-of-range browser_index. With the param
// declared the body runs and answers WEB_BROWSER_NOT_FOUND naming the index; with the
// declaration reverted the dispatcher answers UNKNOWN_PARAMS. No CEF browser is needed: index
// 1000000 never resolves, so the outcome depends only on the param allowlist.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"

namespace DriveBrowserIndexParamTestLocal
{
    // The nine drive verbs whose surface=web body reads browser_index.
    const TArray<FString>& WebDriveVerbs()
    {
        static const TArray<FString> Verbs = {
            TEXT("drive.observe"),
            TEXT("drive.expect"),
            TEXT("drive.wait_for"),
            TEXT("drive.click"),
            TEXT("drive.hover"),
            TEXT("drive.scroll"),
            TEXT("drive.type"),
            TEXT("drive.key"),
            TEXT("drive.drag")
        };
        return Verbs;
    }

    // Minimal valid payload for Verb on surface=web: every key the body validates before
    // selecting the browser, so the only thing left to fail is the browser lookup.
    TSharedPtr<FJsonObject> MakeWebPayload(const FString& Verb, int32 BrowserIndex)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("surface"), TEXT("web"));
        P->SetNumberField(TEXT("browser_index"), BrowserIndex);
        if (Verb == TEXT("drive.expect") || Verb == TEXT("drive.wait_for"))
        {
            TSharedPtr<FJsonObject> Condition = MakeShared<FJsonObject>();
            Condition->SetStringField(TEXT("type"), TEXT("widget_present"));
            Condition->SetStringField(TEXT("target"), TEXT("#pw-no-such-element"));
            P->SetObjectField(TEXT("condition"), Condition);
            // Only wait_for polls; drive.expect is one-shot and does not declare timeout_ms.
            if (Verb == TEXT("drive.wait_for"))
            {
                P->SetNumberField(TEXT("timeout_ms"), 100);
            }
        }
        else if (Verb == TEXT("drive.key"))
        {
            P->SetStringField(TEXT("key"), TEXT("Enter"));
        }
        else if (Verb != TEXT("drive.observe"))
        {
            P->SetStringField(TEXT("handle"), TEXT("#pw-no-such-element"));
        }
        if (Verb == TEXT("drive.type"))
        {
            P->SetStringField(TEXT("text"), TEXT("x"));
        }
        if (Verb == TEXT("drive.drag"))
        {
            P->SetStringField(TEXT("to_handle"), TEXT("#pw-no-such-target"));
        }
        if (Verb == TEXT("drive.observe"))
        {
            P->SetBoolField(TEXT("screenshot"), false);
        }
        return P;
    }
}

// 1. Static: each web drive verb declares browser_index as an optional integer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebVerbsDeclareBrowserIndexTest,
    "PinWright.drive.aliases.WebVerbsDeclareBrowserIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebVerbsDeclareBrowserIndexTest::RunTest(const FString& Parameters)
{
    for (const FString& Verb : DriveBrowserIndexParamTestLocal::WebDriveVerbs())
    {
        const FParamSpec* Spec = ParamSpecTestHelpers::FindParamSpec(Verb, TEXT("browser_index"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares browser_index"), *Verb), Spec))
        {
            continue;
        }
        TestFalse(*FString::Printf(TEXT("%s browser_index is optional"), *Verb), Spec->bRequired);
        TestEqual(*FString::Printf(TEXT("%s browser_index type"), *Verb), Spec->Type, FString(TEXT("integer")));
    }
    return true;
}

// 2. Wire: {surface:"web", browser_index:N} passes the dispatcher on every web verb and reaches
//    the body's browser lookup (WEB_BROWSER_NOT_FOUND naming N), instead of UNKNOWN_PARAMS.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebVerbsAcceptBrowserIndexOnWireTest,
    "PinWright.drive.aliases.WebVerbsAcceptBrowserIndexOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebVerbsAcceptBrowserIndexOnWireTest::RunTest(const FString& Parameters)
{
    const int32 MissIndex = 1000000;
    for (const FString& Verb : DriveBrowserIndexParamTestLocal::WebDriveVerbs())
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Verb,
            FString::Printf(TEXT("req-%s-browser-index"), *Verb),
            DriveBrowserIndexParamTestLocal::MakeWebPayload(Verb, MissIndex), bSuccess, ErrorCode);

        if (!TestTrue(*FString::Printf(TEXT("%s responded synchronously"), *Verb), Sink->bWasCalled))
        {
            continue;
        }
        TestFalse(*FString::Printf(TEXT("%s with an out-of-range browser_index fails"), *Verb), bSuccess);
        TestEqual(*FString::Printf(TEXT("%s reaches the web browser lookup (not UNKNOWN_PARAMS); message: %s"),
                *Verb, *Sink->Message),
            ErrorCode, FString(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND));
        TestTrue(*FString::Printf(TEXT("%s error names the requested browser_index"), *Verb),
            Sink->Message.Contains(FString::Printf(TEXT("browser_index %d"), MissIndex)));
    }
    return true;
}
