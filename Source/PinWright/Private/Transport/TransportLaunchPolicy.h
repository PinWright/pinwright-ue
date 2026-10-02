// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Parse.h"

// Whether this launch binds the checkout's MCP port. An automation run
// (`UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests X;Quit"`) is not a commandlet, so
// without this it would hold the port every MCP session on the checkout needs for its whole run,
// although nothing drives it over MCP: every other session's editor_start / editor_run_tests is
// refused with EDITOR_ALREADY_RUNNING until it exits.
namespace PinWrightTransportLaunchPolicy
{
    // Why the transport must not start for CommandLine, or empty when it should.
    // -PinWrightNoTransport opts any launch out and wins. -PinWrightTransport opts an automation
    // run back in: editor_run_tests passes it because PinWright's own suite exercises the transport.
    // bShouldStopOnSeparator=false keeps a quoted `Automation RunTests A,Quit` whole.
    inline FString SuppressionReason(const TCHAR* CommandLine)
    {
        if (FParse::Param(CommandLine, TEXT("PinWrightNoTransport")))
        {
            return TEXT("-PinWrightNoTransport");
        }
        if (FParse::Param(CommandLine, TEXT("PinWrightTransport")))
        {
            return FString();
        }
        FString ExecCmds;
        if (FParse::Value(CommandLine, TEXT("ExecCmds="), ExecCmds, /*bShouldStopOnSeparator=*/false)
            && ExecCmds.Contains(TEXT("Automation")))
        {
            return TEXT("-ExecCmds automation run without -PinWrightTransport");
        }
        return FString();
    }
}
