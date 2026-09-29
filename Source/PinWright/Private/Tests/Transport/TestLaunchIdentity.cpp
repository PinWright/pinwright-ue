// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins how system.identity reads -PinWrightLaunchReason= / -PinWrightLaunchedBy= (see
// Handlers/System/LaunchIdentity.h). Driven with canned command lines, never by mutating the
// process command line. The cases are the shapes UE actually produces after rebuilding
// FCommandLine from argv: a value with a space arrives quoted, one without arrives bare.

#include "Misc/AutomationTest.h"

#include "Handlers/System/LaunchIdentity.h"

#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwLaunchIdentityParseTest,
    "PinWright.transport.identity.LaunchSwitchesParseAndDecode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwLaunchIdentityParseTest::RunTest(const FString& Parameters)
{
    // Expected == nullptr means the key must be absent.
    auto Check = [this](const TCHAR* CommandLine, const TCHAR* Key, const TCHAR* Expected)
    {
        FJsonObject Result;
        PinWrightLaunchIdentity::WriteLaunchFields(CommandLine, Result);
        FString Actual;
        const bool bPresent = Result.TryGetStringField(Key, Actual);
        const FString What = FString::Printf(TEXT("%s from [%s]"), Key, CommandLine);
        if (Expected == nullptr)
        {
            TestFalse(*(What + TEXT(" is omitted")), bPresent);
        }
        else
        {
            TestTrue(*(What + TEXT(" is present")), bPresent);
            TestEqual(*What, Actual, FString(Expected));
        }
    };

    // Quoted value keeps its space, comma and parens: the default separator-stopping parse would
    // cut it at the comma.
    Check(TEXT("X.uproject -PinWrightLaunchReason=\"fix A, (B)\" -log"),
        TEXT("launch_reason"), TEXT("fix A, (B)"));
    // Unquoted value (no space) keeps its comma.
    Check(TEXT("X.uproject -PinWrightLaunchReason=fix,A -log"), TEXT("launch_reason"), TEXT("fix,A"));
    // Encoded quotes and percent decode.
    Check(TEXT("X.uproject -PinWrightLaunchReason=\"%22x%22 100%25\""),
        TEXT("launch_reason"), TEXT("\"x\" 100%"));
    // One pass only: %2522 is an encoded "%22", not a double-encoded quote.
    Check(TEXT("X.uproject -PinWrightLaunchReason=%2522"), TEXT("launch_reason"), TEXT("%22"));
    // A `%` that is not one of the two escapes stays literal.
    Check(TEXT("X.uproject -PinWrightLaunchReason=50%off%2"), TEXT("launch_reason"), TEXT("50%off%2"));
    // Trailing backslash inside quotes is data, not an escape of the closing quote.
    Check(TEXT("X.uproject -PinWrightLaunchReason=\"C:\\dir\\\" -log"),
        TEXT("launch_reason"), TEXT("C:\\dir\\"));
    // Non-ASCII survives untouched.
    Check(TEXT("X.uproject -PinWrightLaunchReason=\"\u043F\u0440\u0438\u0432\u043E\u0434 \u2713\""),
        TEXT("launch_reason"), TEXT("\u043F\u0440\u0438\u0432\u043E\u0434 \u2713"));

    Check(TEXT("X.uproject -PinWrightLaunchedBy=editor_start -log"), TEXT("launched_by"), TEXT("editor_start"));
    Check(TEXT("X.uproject -PinWrightLaunchedBy=editor_start -log"), TEXT("launch_reason"), nullptr);

    // Absent switches: both keys omitted, not reported empty.
    Check(TEXT("X.uproject -log -unattended"), TEXT("launch_reason"), nullptr);
    Check(TEXT("X.uproject -log -unattended"), TEXT("launched_by"), nullptr);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
