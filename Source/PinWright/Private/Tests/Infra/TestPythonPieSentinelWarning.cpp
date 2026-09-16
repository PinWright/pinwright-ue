// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/UnrealTemplate.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonExecutePieSentinelWarningTest,
    "PinWright.python.execute.PieSentinelWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual: without the handler's shared PIE-state check, the guarded call below has
// neither pieActive nor the sentinel warning and both response-contract assertions fail.
bool FPythonExecutePieSentinelWarningTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("code"), TEXT("2 + 2"));
    Payload->SetStringField(TEXT("mode"), TEXT("evaluate_statement"));

    FTestResponseCapture Capture;
    {
        TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
        if (!InvokeHandlerWithCapture(TEXT("python.execute"), Payload, Capture))
        {
            AddError(TEXT("Handler 'python.execute' not registered"));
            return false;
        }
    }

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        if (Capture.ErrorCode == TEXT("PYTHON_NOT_AVAILABLE") ||
            Capture.ErrorCode == TEXT("PYTHON_INIT_FAILED"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("python.execute answered %s"), *Capture.ErrorCode));
            return true;
        }

        AddError(FString::Printf(TEXT("python.execute failed (errorCode='%s')"), *Capture.ErrorCode));
        return false;
    }

    bool bScriptSucceeded = false;
    TestTrue(TEXT("Python expression executes successfully"),
        Capture.Result->TryGetBoolField(TEXT("success"), bScriptSucceeded) && bScriptSucceeded);

    bool bPieActive = false;
    TestTrue(TEXT("Response identifies PIE as active"),
        Capture.Result->TryGetBoolField(TEXT("pieActive"), bPieActive) && bPieActive);

    const TArray<TSharedPtr<FJsonValue>>* LogEntries = nullptr;
    TestTrue(TEXT("Response carries the Python log array"),
        Capture.Result->TryGetArrayField(TEXT("log"), LogEntries) && LogEntries);

    bool bFoundSentinelWarning = false;
    if (LogEntries)
    {
        for (const TSharedPtr<FJsonValue>& EntryValue : *LogEntries)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (!EntryValue.IsValid() || !EntryValue->TryGetObject(Entry) || !Entry)
            {
                continue;
            }

            FString Type;
            FString Output;
            (*Entry)->TryGetStringField(TEXT("type"), Type);
            (*Entry)->TryGetStringField(TEXT("output"), Output);
            if (Type == TEXT("Warning") &&
                Output.Contains(TEXT("StaticMeshEditorSubsystem.get_lod_count")) &&
                Output.Contains(TEXT("get_num_uv_channels")) &&
                Output.Contains(TEXT("sentinels")))
            {
                bFoundSentinelWarning = true;
                break;
            }
        }
    }

    TestTrue(TEXT("PIE response warns that static-mesh subsystem values are sentinels"),
        bFoundSentinelWarning);
    return true;
}
