// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-python-bp-dispatcher-property-unsafe.
//
// The engine's Python wrapper for a Blueprint event-dispatcher property answered
// is_bound() == False on a bound dispatcher, and dir() on it SIGSEGV'd the editor.
// PinWright cannot patch the engine plugin, so python.execute warns whenever a script
// calls is_bound() and the python page lists the hazard with the typed alternative.
//
// Counterfactual: removing the handler's is_bound scan drops the Warning entry and the
// first test fails; deleting the python.md paragraph fails the doc test.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Tests/Infra/WikiDocTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

namespace
{
    // Returns false (after skipping or erroring) when python.execute could not run.
    bool RunPythonForDispatcherWarning(FAutomationTestBase& Test, const FString& Code, bool& bOutWarned)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("code"), Code);
        Payload->SetStringField(TEXT("mode"), TEXT("evaluate_statement"));

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("python.execute"), Payload, Capture))
        {
            Test.AddError(TEXT("Handler 'python.execute' not registered"));
            return false;
        }
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            if (Capture.ErrorCode == TEXT("PYTHON_NOT_AVAILABLE") ||
                Capture.ErrorCode == TEXT("PYTHON_INIT_FAILED"))
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("python-interpreter-unavailable"),
                    FString::Printf(TEXT("python.execute answered %s"), *Capture.ErrorCode));
                return false;
            }
            Test.AddError(FString::Printf(TEXT("python.execute failed (errorCode='%s')"), *Capture.ErrorCode));
            return false;
        }

        bOutWarned = false;
        const TArray<TSharedPtr<FJsonValue>>* LogEntries = nullptr;
        Capture.Result->TryGetArrayField(TEXT("log"), LogEntries);
        for (const TSharedPtr<FJsonValue>& EntryValue : LogEntries ? *LogEntries : TArray<TSharedPtr<FJsonValue>>())
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString Type;
            FString Output;
            if (EntryValue.IsValid() && EntryValue->TryGetObject(Entry) && Entry &&
                (*Entry)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("Warning") &&
                (*Entry)->TryGetStringField(TEXT("output"), Output) &&
                Output.Contains(TEXT("is_bound()")) && Output.Contains(TEXT("property.get")) &&
                Output.Contains(TEXT("bindings[]")))
            {
                bOutWarned = true;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonExecuteIsBoundDispatcherWarningTest,
    "PinWright.python.execute.IsBoundDispatcherWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonExecuteIsBoundDispatcherWarningTest::RunTest(const FString& Parameters)
{
    // The scan is textual, so a string literal is enough to trigger it without touching
    // a real delegate wrapper (which is exactly what must not be done here).
    bool bWarned = false;
    if (!RunPythonForDispatcherWarning(*this, TEXT("'w.get_editor_property(\"OnFoo\").is_bound()'"), bWarned))
    {
        return true;
    }
    TestTrue(TEXT("a script that calls is_bound() is warned toward property.get"), bWarned);

    bool bWarnedWithout = true;
    if (!RunPythonForDispatcherWarning(*this, TEXT("2 + 2"), bWarnedWithout))
    {
        return true;
    }
    TestFalse(TEXT("a script without is_bound() carries no dispatcher warning"), bWarnedWithout);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonDispatcherHazardDocTest,
    "PinWright.infra.wiki_handler.NamespacePage.PythonDispatcherHazard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonDispatcherHazardDocTest::RunTest(const FString& Parameters)
{
    FString PythonText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("python"), PythonText))
    {
        return false;
    }

    const int32 CrashSection = PythonText.Find(TEXT("## Calls that crash the editor"));
    const int32 FreezeSection = PythonText.Find(TEXT("## Calls that freeze the editor"));
    const int32 Hazard = PythonText.Find(TEXT("event-dispatcher"));
    TestTrue(TEXT("dispatcher hazard is listed under 'Calls that crash the editor'"),
        CrashSection != INDEX_NONE && Hazard > CrashSection && (FreezeSection == INDEX_NONE || Hazard < FreezeSection));
    TestTrue(TEXT("python page names get_editor_property and is_bound()"),
        PythonText.Contains(TEXT("get_editor_property")) && PythonText.Contains(TEXT("is_bound()")));
    TestTrue(TEXT("python page routes dispatcher reads to property.get bindings"),
        PythonText.Contains(TEXT("bindingStatus")) && PythonText.Contains(TEXT("bindings[]")));
    return true;
}
