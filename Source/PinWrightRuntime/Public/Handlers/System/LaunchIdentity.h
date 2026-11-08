// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/Parse.h"

// Why and by what this editor was launched, as stated by the PinWright proxy tool that started it
// (the stdio proxy's editor_start / editor_restart / editor_run_tests).
// These are ECHOED launch arguments, not measurements: they are deliberately kept out of
// PinWrightEditorIdentity::FIdentity and are not assertable, because any process launching the editor can pass them.
namespace PinWrightLaunchIdentity
{
    // Reverses the proxy's encoding in ONE pass: `%22` -> `"`, `%25` -> `%`, any other `%`
    // stays literal, so `%2522` decodes to `%22` and is not decoded again. The proxy escapes
    // `"` because FParse::Value ends a quoted value at the first `"` with no escape for it
    // (Parse.cpp:278-290), and UE rebuilds the command line as `-Key="value"` whenever the value
    // holds a space (LaunchWindows.cpp:147-181, UnixCommonStartup.cpp:263-281).
    inline FString DecodeLaunchReason(const FString& Encoded)
    {
        FString Out;
        Out.Reserve(Encoded.Len());
        for (int32 i = 0; i < Encoded.Len(); ++i)
        {
            if (Encoded[i] == TEXT('%') && i + 2 < Encoded.Len() &&Encoded[i + 1] == TEXT('2'))
            {
                if (Encoded[i + 2] == TEXT('2')) { Out.AppendChar(TEXT('"')); i += 2; continue; }
                if (Encoded[i + 2] == TEXT('5')) { Out.AppendChar(TEXT('%')); i += 2; continue; }
            }
            Out.AppendChar(Encoded[i]);
        }
        return Out;
    }

    // Writes `launch_reason` (decoded) and `launched_by` into Result, each omitted when its switch
    // is absent from CommandLine. bShouldStopOnSeparator=false is load-bearing: the default stops
    // an unquoted value at `,` or `)` (Parse.cpp:299), truncating a reason like `fix A,B`.
    inline void WriteLaunchFields(const TCHAR* CommandLine, FJsonObject& Result)
    {
        FString Value;
        if (FParse::Value(CommandLine, TEXT("PinWrightLaunchReason="), Value, /*bShouldStopOnSeparator=*/false))
        {
            Result.SetStringField(TEXT("launch_reason"), DecodeLaunchReason(Value));
        }
        if (FParse::Value(CommandLine, TEXT("PinWrightLaunchedBy="), Value, /*bShouldStopOnSeparator=*/false))
        {
            Result.SetStringField(TEXT("launched_by"), Value);
        }
    }
}
