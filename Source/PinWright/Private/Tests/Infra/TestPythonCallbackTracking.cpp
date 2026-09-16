// Copyright (c) 2026 Alexander Penkin. MIT License.

// End-to-end proof for the python.callbacks tracker, driven through the two verbs a caller
// would use and measured against the ENGINE's registry rather than the plugin's bookkeeping.
//
// The counterfactual the last assertion carries: the probe's own tick counter must not move
// on a Slate post-tick broadcast issued AFTER the clear. If RequestClear stopped reaching
// unreal.unregister_slate_post_tick_callback -- or the shim stopped holding the engine
// handle that call needs -- the record would still disappear from the plugin's registry and
// every other assertion here would still pass, while the callback kept running. Broadcasting
// the event by hand is what separates "PinWright forgot it" from "the editor stopped calling
// it", which is the whole claim of the verb.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

namespace PythonCallbackTrackingTest
{
    // Global names the probe script binds in the UE Python console namespace. Removed by the
    // test's own teardown script so a later python test does not inherit them.
    const TCHAR* const TickCounterName = TEXT("PW_CB_TRACK_TICKS");
    const TCHAR* const ProbeName = TEXT("pw_cb_track_probe");

    TSharedPtr<FJsonObject> ListPayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("action"), TEXT("list"));
        return Payload;
    }

    // Defaults to public scope on purpose: the tick probe's counter has to stay readable by
    // a later evaluate_statement call, which is how the engine-side assertion is measured.
    // The retained-script test overrides it, because only private scope writes a temp file.
    TSharedPtr<FJsonObject> ExecutePayload(const FString& Code, const FString& Mode,
        const FString& Scope = TEXT("public"))
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("code"), Code);
        Payload->SetStringField(TEXT("mode"), Mode);
        Payload->SetStringField(TEXT("scope"), Scope);
        return Payload;
    }

    TSharedPtr<FJsonObject> ClearPayloadFor(const FString& CallbackId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("action"), TEXT("clear"));
        TArray<TSharedPtr<FJsonValue>> Ids;
        Ids.Add(MakeShared<FJsonValueString>(CallbackId));
        Payload->SetArrayField(TEXT("ids"), Ids);
        return Payload;
    }

    bool IsHostLimitationCode(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("PYTHON_NOT_AVAILABLE")
            || ErrorCode == TEXT("PYTHON_INIT_FAILED")
            || ErrorCode == TEXT("PYTHON_CALLBACK_TRACKING_UNAVAILABLE");
    }

    TArray<TSharedPtr<FJsonObject>> ReadCallbacks(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<TSharedPtr<FJsonObject>> Entries;
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("callbacks"), Values) || !Values)
        {
            return Entries;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry)
            {
                Entries.Add(*Entry);
            }
        }
        return Entries;
    }

    TSet<FString> ReadIds(const TSharedPtr<FJsonObject>& Result)
    {
        TSet<FString> Ids;
        for (const TSharedPtr<FJsonObject>& Entry : ReadCallbacks(Result))
        {
            FString Id;
            if (Entry->TryGetStringField(TEXT("id"), Id))
            {
                Ids.Add(Id);
            }
        }
        return Ids;
    }

    TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        TArray<FString> Out;
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(Field, Values) || !Values)
        {
            return Out;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text))
            {
                Out.Add(Text);
            }
        }
        return Out;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonCallbackTrackingTest,
    "PinWright.python.callbacks.LeakedTickCallbackIsListedAndCleared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonCallbackTrackingTest::RunTest(const FString& Parameters)
{
    namespace PwCbTest = PythonCallbackTrackingTest;

    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate-not-initialized"),
            TEXT("register_slate_post_tick_callback registers nothing without FSlateApplication"));
        return true;
    }

    auto ListTrackedCallbacks = [this](TSharedPtr<FJsonObject>& OutResult, FString& OutErrorCode) -> bool
    {
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("python.callbacks"), PwCbTest::ListPayload(), Capture))
        {
            AddError(TEXT("Handler 'python.callbacks' not registered"));
            return false;
        }
        OutErrorCode = Capture.ErrorCode;
        OutResult = Capture.Result;
        return Capture.bSuccess && Capture.Result.IsValid();
    };

    // This first call is also what installs the tracker, so a host without a usable
    // interpreter is detected here rather than halfway through the fixture.
    TSharedPtr<FJsonObject> BaselineList;
    FString BaselineErrorCode;
    if (!ListTrackedCallbacks(BaselineList, BaselineErrorCode))
    {
        if (PwCbTest::IsHostLimitationCode(BaselineErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-callback-tracking-unavailable"),
                FString::Printf(TEXT("python.callbacks answered %s"), *BaselineErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("python.callbacks list failed (errorCode='%s')"), *BaselineErrorCode));
        return false;
    }
    const TSet<FString> IdsBefore = PwCbTest::ReadIds(BaselineList);

    const FString RegisterScript = FString::Printf(
        TEXT("import unreal\n")
        TEXT("%s = [0]\n")
        TEXT("def %s(delta_seconds):\n")
        TEXT("    %s[0] += 1\n")
        TEXT("unreal.register_slate_post_tick_callback(%s)\n"),
        PwCbTest::TickCounterName, PwCbTest::ProbeName, PwCbTest::TickCounterName, PwCbTest::ProbeName);

    FTestResponseCapture RegisterCapture;
    if (!InvokeHandlerWithCapture(TEXT("python.execute"),
            PwCbTest::ExecutePayload(RegisterScript, TEXT("execute_file")), RegisterCapture))
    {
        AddError(TEXT("Handler 'python.execute' not registered"));
        return false;
    }
    if (!RegisterCapture.bSuccess || !RegisterCapture.Result.IsValid())
    {
        if (PwCbTest::IsHostLimitationCode(RegisterCapture.ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("python.execute answered %s"), *RegisterCapture.ErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("python.execute failed (errorCode='%s')"), *RegisterCapture.ErrorCode));
        return false;
    }

    // Registered from here on: the callback is live in the editor's Slate post-tick list and
    // must be removed on every exit path, or it ticks through the rest of the suite.
    FString CallbackId;
    ON_SCOPE_EXIT
    {
        if (!CallbackId.IsEmpty())
        {
            FTestResponseCapture Discard;
            InvokeHandlerWithCapture(TEXT("python.callbacks"),
                PwCbTest::ClearPayloadFor(CallbackId), Discard);
        }

        const FString TeardownScript = FString::Printf(
            TEXT("globals().pop('%s', None)\n")
            TEXT("globals().pop('%s', None)\n"),
            PwCbTest::TickCounterName, PwCbTest::ProbeName);
        FTestResponseCapture Discard;
        InvokeHandlerWithCapture(TEXT("python.execute"),
            PwCbTest::ExecutePayload(TeardownScript, TEXT("execute_file")), Discard);
    };

    bool bScriptSucceeded = false;
    TestTrue(TEXT("registration script runs"),
        RegisterCapture.Result->TryGetBoolField(TEXT("success"), bScriptSucceeded) && bScriptSucceeded);

    double LeakedCallbacks = 0.0;
    TestTrue(TEXT("python.execute reports the callback its script left registered"),
        RegisterCapture.Result->TryGetNumberField(TEXT("leakedCallbacks"), LeakedCallbacks)
            && LeakedCallbacks >= 1.0);

    TSharedPtr<FJsonObject> ListAfterRegister;
    FString ListErrorCode;
    if (!ListTrackedCallbacks(ListAfterRegister, ListErrorCode))
    {
        AddError(FString::Printf(TEXT("python.callbacks list failed after registering (errorCode='%s')"),
            *ListErrorCode));
        return false;
    }

    TArray<FString> NewIds;
    for (const FString& Id : PwCbTest::ReadIds(ListAfterRegister))
    {
        if (!IdsBefore.Contains(Id))
        {
            NewIds.Add(Id);
        }
    }
    if (NewIds.Num() != 1)
    {
        AddError(FString::Printf(
            TEXT("Expected exactly one newly tracked callback, saw %d"), NewIds.Num()));
        return false;
    }
    CallbackId = NewIds[0];

    for (const TSharedPtr<FJsonObject>& Entry : PwCbTest::ReadCallbacks(ListAfterRegister))
    {
        FString Id;
        if (!Entry->TryGetStringField(TEXT("id"), Id) || Id != CallbackId)
        {
            continue;
        }
        FString Kind;
        Entry->TryGetStringField(TEXT("kind"), Kind);
        TestEqual(TEXT("the tracked callback is a Slate post-tick registration"),
            Kind, FString(TEXT("slate_post_tick")));

        FString Slot;
        Entry->TryGetStringField(TEXT("slot"), Slot);
        TestFalse(TEXT("the record names the request that registered it"), Slot.IsEmpty());

        FString Source;
        Entry->TryGetStringField(TEXT("source"), Source);
        TestTrue(TEXT("the record names the registering source line"),
            Source.Contains(PwCbTest::ProbeName) || Source.Contains(TEXT(":")));
    }

    auto ReadTickCount = [this](int32& OutTicks) -> bool
    {
        FTestResponseCapture Capture;
        const FString Expression = FString::Printf(TEXT("%s[0]"), PwCbTest::TickCounterName);
        if (!InvokeHandlerWithCapture(TEXT("python.execute"),
                PwCbTest::ExecutePayload(Expression, TEXT("evaluate_statement")), Capture))
        {
            AddError(TEXT("Handler 'python.execute' not registered"));
            return false;
        }
        bool bEvaluated = false;
        if (!Capture.bSuccess || !Capture.Result.IsValid()
            || !Capture.Result->TryGetBoolField(TEXT("success"), bEvaluated) || !bEvaluated)
        {
            AddError(TEXT("Could not read the probe's tick counter back from Python"));
            return false;
        }
        FString ResultText;
        Capture.Result->TryGetStringField(TEXT("result"), ResultText);
        OutTicks = FCString::Atoi(*ResultText);
        return true;
    };

    FSlateApplication::Get().OnPostTick().Broadcast(0.0f);

    int32 TicksWhileRegistered = 0;
    if (!ReadTickCount(TicksWhileRegistered))
    {
        return false;
    }
    if (TicksWhileRegistered == 0)
    {
        // PyUtil::CanTickPython() refuses to enter Python during GC, package save and while a
        // script is already on the stack, so a host that answers zero here has not disproved
        // anything - it has failed to set up the measurement.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate-post-tick-did-not-reach-python"),
            TEXT("a manual OnPostTick broadcast did not invoke the registered Python callback"));
        return true;
    }
    TestTrue(TEXT("the registered callback runs on a Slate post tick"), TicksWhileRegistered >= 1);

    TSharedPtr<FJsonObject> ListAfterTick;
    if (ListTrackedCallbacks(ListAfterTick, ListErrorCode))
    {
        for (const TSharedPtr<FJsonObject>& Entry : PwCbTest::ReadCallbacks(ListAfterTick))
        {
            FString Id;
            if (Entry->TryGetStringField(TEXT("id"), Id) && Id == CallbackId)
            {
                double Invocations = 0.0;
                Entry->TryGetNumberField(TEXT("invocations"), Invocations);
                TestTrue(TEXT("the record counts the invocation"), Invocations >= 1.0);
            }
        }
    }

    FTestResponseCapture ClearCapture;
    if (!InvokeHandlerWithCapture(TEXT("python.callbacks"),
            PwCbTest::ClearPayloadFor(CallbackId), ClearCapture))
    {
        AddError(TEXT("Handler 'python.callbacks' not registered"));
        return false;
    }
    if (!ClearCapture.bSuccess || !ClearCapture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("python.callbacks clear failed (errorCode='%s')"),
            *ClearCapture.ErrorCode));
        return false;
    }
    TestTrue(TEXT("clear reports the callback as cleared"),
        PwCbTest::ReadStringArray(ClearCapture.Result, TEXT("cleared")).Contains(CallbackId));
    TestFalse(TEXT("clear does not report the callback as failed"),
        PwCbTest::ReadStringArray(ClearCapture.Result, TEXT("failed")).Contains(CallbackId));

    // THE COUNTERFACTUAL. Same broadcast as above, after the clear.
    FSlateApplication::Get().OnPostTick().Broadcast(0.0f);

    int32 TicksAfterClear = 0;
    if (!ReadTickCount(TicksAfterClear))
    {
        return false;
    }
    TestEqual(TEXT("the engine no longer invokes the cleared callback"),
        TicksAfterClear, TicksWhileRegistered);

    TSharedPtr<FJsonObject> FinalList;
    if (!ListTrackedCallbacks(FinalList, ListErrorCode))
    {
        AddError(FString::Printf(TEXT("python.callbacks list failed after clearing (errorCode='%s')"),
            *ListErrorCode));
        return false;
    }
    TestFalse(TEXT("the cleared callback is gone from the listing"),
        PwCbTest::ReadIds(FinalList).Contains(CallbackId));

    // CallbackId is deliberately left set: the scope-exit clear is a no-op on an id that is
    // already gone, and leaving it armed is what covers an assertion failure above.
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonCallbackRetainedScriptTest,
    "PinWright.python.callbacks.LeakedPrivateScriptStaysOnDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A private inline script runs from a temp file, and the source line every leaked callback
// reports names that file. Deleting it unconditionally is what made the recorded incident
// unattributable: 94,358 log lines quoted InlinePython_<guid>.py with the file already gone.
// Counterfactual: restore the unconditional delete in PythonExecuteHandler.cpp and both
// `retainedScript` and the on-disk existence check below fail, while every other assertion
// in this file still passes.
bool FPythonCallbackRetainedScriptTest::RunTest(const FString& Parameters)
{
    namespace PwCbTest = PythonCallbackTrackingTest;

    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate-not-initialized"),
            TEXT("register_slate_post_tick_callback registers nothing without FSlateApplication"));
        return true;
    }

    auto ListTracked = [this](TSharedPtr<FJsonObject>& OutResult, FString& OutErrorCode) -> bool
    {
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("python.callbacks"), PwCbTest::ListPayload(), Capture))
        {
            AddError(TEXT("Handler 'python.callbacks' not registered"));
            return false;
        }
        OutErrorCode = Capture.ErrorCode;
        OutResult = Capture.Result;
        return Capture.bSuccess && Capture.Result.IsValid();
    };

    TSharedPtr<FJsonObject> BaselineList;
    FString ErrorCode;
    if (!ListTracked(BaselineList, ErrorCode))
    {
        if (PwCbTest::IsHostLimitationCode(ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-callback-tracking-unavailable"),
                FString::Printf(TEXT("python.callbacks answered %s"), *ErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("python.callbacks list failed (errorCode='%s')"), *ErrorCode));
        return false;
    }
    const TSet<FString> IdsBefore = PwCbTest::ReadIds(BaselineList);

    // Private scope is the whole point: it is the mode that routes an inline script through
    // a temp file. The probe body does nothing, so the only thing under test is the file.
    const TCHAR* const RegisterScript =
        TEXT("import unreal\n")
        TEXT("def pw_cb_retained_probe(delta_seconds):\n")
        TEXT("    pass\n")
        TEXT("unreal.register_slate_post_tick_callback(pw_cb_retained_probe)\n");

    FTestResponseCapture RegisterCapture;
    if (!InvokeHandlerWithCapture(TEXT("python.execute"),
            PwCbTest::ExecutePayload(RegisterScript, TEXT("execute_file"), TEXT("private")),
            RegisterCapture))
    {
        AddError(TEXT("Handler 'python.execute' not registered"));
        return false;
    }
    if (!RegisterCapture.bSuccess || !RegisterCapture.Result.IsValid())
    {
        if (PwCbTest::IsHostLimitationCode(RegisterCapture.ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("python.execute answered %s"), *RegisterCapture.ErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("python.execute failed (errorCode='%s')"),
            *RegisterCapture.ErrorCode));
        return false;
    }

    FString RetainedScript;
    FString CallbackId;
    ON_SCOPE_EXIT
    {
        if (!CallbackId.IsEmpty())
        {
            FTestResponseCapture Discard;
            InvokeHandlerWithCapture(TEXT("python.callbacks"),
                PwCbTest::ClearPayloadFor(CallbackId), Discard);
        }
        // The retained script is deliberately not cleaned up by the plugin, so the fixture
        // owns it; leaving it would accumulate a file per suite run.
        if (!RetainedScript.IsEmpty())
        {
            IFileManager::Get().Delete(*RetainedScript, /*RequireExists=*/false, /*EvenReadOnly=*/true);
        }
    };

    double LeakedCallbacks = 0.0;
    if (!RegisterCapture.Result->TryGetNumberField(TEXT("leakedCallbacks"), LeakedCallbacks)
        || LeakedCallbacks < 1.0)
    {
        AddError(TEXT("python.execute did not report the callback the private script left registered"));
        return false;
    }

    RegisterCapture.Result->TryGetStringField(TEXT("retainedScript"), RetainedScript);
    if (!TestFalse(TEXT("a leaking private call reports the script it kept"), RetainedScript.IsEmpty()))
    {
        return false;
    }
    TestTrue(TEXT("the retained script is still on disk"),
        IFileManager::Get().FileExists(*RetainedScript));

    TSharedPtr<FJsonObject> ListAfter;
    if (!ListTracked(ListAfter, ErrorCode))
    {
        AddError(FString::Printf(TEXT("python.callbacks list failed after registering (errorCode='%s')"),
            *ErrorCode));
        return false;
    }

    // Claim the id BEFORE asserting anything about it, so the scope-exit clear is armed on
    // every failure path - an unclaimed probe would tick through the rest of the suite.
    FString LeakedSource;
    for (const TSharedPtr<FJsonObject>& Entry : PwCbTest::ReadCallbacks(ListAfter))
    {
        FString Id;
        if (!Entry->TryGetStringField(TEXT("id"), Id) || IdsBefore.Contains(Id))
        {
            continue;
        }
        CallbackId = Id;
        Entry->TryGetStringField(TEXT("source"), LeakedSource);
        break;
    }
    if (!TestFalse(TEXT("the private call's callback is tracked"), CallbackId.IsEmpty()))
    {
        return false;
    }

    // The claim being locked: the source line the record reports resolves to a file that
    // still exists. Matching on the clean filename rather than the whole path keeps the
    // assertion off Python's own spelling of the path it was handed.
    TestTrue(TEXT("the leaked record names the retained script as its source"),
        LeakedSource.Contains(FPaths::GetCleanFilename(RetainedScript)));
    return true;
}
