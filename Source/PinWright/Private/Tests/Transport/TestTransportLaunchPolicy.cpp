// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins which launches bind the checkout's MCP port (Transport/TransportLaunchPolicy.h). Driven with
// canned command lines in the shapes UE rebuilds FCommandLine into: a value with a space arrives
// quoted, one without arrives bare.

#include "Misc/AutomationTest.h"

#include "Transport/TransportLaunchPolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTransportLaunchPolicyTest,
    "PinWright.transport.launch_policy.AutomationRunLeavesThePortFree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwTransportLaunchPolicyTest::RunTest(const FString& Parameters)
{
    auto Binds = [this](const TCHAR* CommandLine, bool bExpected)
    {
        const bool bActual = PinWrightTransportLaunchPolicy::SuppressionReason(CommandLine).IsEmpty();
        TestEqual(*FString::Printf(TEXT("binds for [%s]"), CommandLine), bActual, bExpected);
    };

    // Interactive and editor_start launches keep the transport, including -RunningUnattendedScript.
    Binds(TEXT("X.uproject -log"), true);
    Binds(TEXT("X.uproject -RenderOffScreen -unattended -RunningUnattendedScript -nopause"), true);
    Binds(TEXT("X.uproject -ExecCmds=\"stat fps\""), true);

    // A plain automation run leaves the port free, whatever the separator or casing.
    Binds(TEXT("X.uproject -ExecCmds=\"Automation RunTests Project.Foo;Quit\" -TestExit=\"Automation Test Queue Empty\" -unattended"), false);
    Binds(TEXT("X.uproject -execcmds=\"automation runtests A,Quit\" -RunningUnattendedScript"), false);
    Binds(TEXT("X.uproject -ExecCmds=Automation"), false);

    // editor_run_tests opts PinWright's own suite back in.
    Binds(TEXT("X.uproject -ExecCmds=\"Automation RunTests PinWright,Quit\" -PinWrightTransport"), true);

    // -PinWrightNoTransport opts any launch out and beats the opt-in.
    Binds(TEXT("X.uproject -PinWrightNoTransport"), false);
    Binds(TEXT("X.uproject -PinWrightTransport -PinWrightNoTransport"), false);

    // A longer switch sharing the prefix is not the opt-in.
    Binds(TEXT("X.uproject -ExecCmds=\"Automation RunTests A\" -PinWrightTransportX"), false);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
