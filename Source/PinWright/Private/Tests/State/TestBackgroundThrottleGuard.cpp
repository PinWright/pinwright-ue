// Copyright (c) 2026 Alexander Penkin. MIT License.

// FBackgroundThrottleGuard: the two engine levers (the ShouldDisableCPUThrottlingDelegates
// entry and the in-memory bThrottleCPUWhenNotForeground flag) move together with agent
// activity, stay put with the setting off, and always hand the user's value back.
//
// Each test drives its own guard instance against the real GEditor delegate array and
// the real UEditorPerformanceSettings CDO, and restores the CDO flag it found. The
// subsystem's own guard may be holding concurrently; it does not tick inside a test body.
#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorPerformanceSettings.h"
#include "State/BackgroundThrottleGuard.h"
#include "State/ClientActivity.h"

namespace PwBackgroundThrottleTest
{
    // Executes this guard's entry in the engine's delegate array. OutFound reports
    // whether the entry is registered at all, so "answers false" and "is gone" stay apart.
    static bool ExecuteGuardDelegate(const FBackgroundThrottleGuard& Guard, bool& OutFound)
    {
        OutFound = false;
        for (const UEditorEngine::FShouldDisableCPUThrottling& Delegate : GEditor->ShouldDisableCPUThrottlingDelegates)
        {
            if (Delegate.GetHandle() == Guard.GetThrottleDelegateHandle() && Delegate.IsBound())
            {
                OutFound = true;
                return Delegate.Execute();
            }
        }
        return false;
    }

    struct FScopedThrottleFlag
    {
        bool bOriginal;
        explicit FScopedThrottleFlag(bool bValue)
            : bOriginal(GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground)
        {
            GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = bValue;
        }
        ~FScopedThrottleFlag()
        {
            GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = bOriginal;
        }
    };

    static bool ThrottleFlag()
    {
        return GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBackgroundThrottleActivityTogglesTest,
    "PinWright.state.background_throttle.ActivityTogglesDelegateAndFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackgroundThrottleActivityTogglesTest::RunTest(const FString& Parameters)
{
    using namespace PwBackgroundThrottleTest;
    if (!TestNotNull(TEXT("GEditor"), GEditor))
    {
        return false;
    }

    FScopedThrottleFlag Flag(true);
    FBackgroundThrottleGuard Guard;
    Guard.Register();

    bool bFound = false;
    TestFalse(TEXT("idle: delegate answers false"), ExecuteGuardDelegate(Guard, bFound));
    TestTrue(TEXT("Register adds an entry to ShouldDisableCPUThrottlingDelegates"), bFound);
    TestTrue(TEXT("idle: flag untouched"), ThrottleFlag());

    Guard.Update(/*bSettingEnabled=*/true, /*bAgentActive=*/true);
    TestTrue(TEXT("active: delegate disables CPU throttling"), ExecuteGuardDelegate(Guard, bFound));
    TestFalse(TEXT("active: flag cleared so the viewport override stays off"), ThrottleFlag());

    Guard.Update(true, false);
    TestFalse(TEXT("lapsed: delegate answers false"), ExecuteGuardDelegate(Guard, bFound));
    TestTrue(TEXT("lapsed: user's flag restored"), ThrottleFlag());

    Guard.Unregister();
    ExecuteGuardDelegate(Guard, bFound);
    TestFalse(TEXT("Unregister removes the entry"), bFound);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBackgroundThrottleSettingOffTest,
    "PinWright.state.background_throttle.SettingOffLeavesDelegateAndFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackgroundThrottleSettingOffTest::RunTest(const FString& Parameters)
{
    using namespace PwBackgroundThrottleTest;
    if (!TestNotNull(TEXT("GEditor"), GEditor))
    {
        return false;
    }

    for (const bool bUserValue : {true, false})
    {
        FScopedThrottleFlag Flag(bUserValue);
        FBackgroundThrottleGuard Guard;
        Guard.Register();

        Guard.Update(/*bSettingEnabled=*/false, /*bAgentActive=*/true);
        bool bFound = false;
        TestFalse(FString::Printf(TEXT("user=%d: delegate answers false with the setting off"), bUserValue),
            ExecuteGuardDelegate(Guard, bFound));
        TestTrue(TEXT("entry registered"), bFound);
        TestEqual(FString::Printf(TEXT("user=%d: flag untouched with the setting off"), bUserValue),
            ThrottleFlag(), bUserValue);
        TestFalse(TEXT("not holding"), Guard.IsHolding());

        // Turning the setting off mid-hold releases on the next update.
        Guard.Update(true, true);
        TestTrue(TEXT("holding once enabled"), Guard.IsHolding());
        Guard.Update(false, true);
        TestFalse(TEXT("setting off releases the hold"), ExecuteGuardDelegate(Guard, bFound));
        TestEqual(TEXT("setting off restores the user's flag"), ThrottleFlag(), bUserValue);

        Guard.Unregister();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBackgroundThrottleRestoresUserValueTest,
    "PinWright.state.background_throttle.UnregisterRestoresUserValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackgroundThrottleRestoresUserValueTest::RunTest(const FString& Parameters)
{
    using namespace PwBackgroundThrottleTest;
    if (!TestNotNull(TEXT("GEditor"), GEditor))
    {
        return false;
    }

    // Both directions: a restore that always wrote `true` (the engine default) would pass
    // the first case alone.
    for (const bool bUserValue : {true, false})
    {
        FScopedThrottleFlag Flag(bUserValue);
        {
            FBackgroundThrottleGuard Guard;
            Guard.Register();
            Guard.Update(true, true);
            TestFalse(TEXT("held flag reads false"), ThrottleFlag());
            Guard.Unregister();
            TestEqual(FString::Printf(TEXT("user=%d: Unregister mid-hold restores the user's value"), bUserValue),
                ThrottleFlag(), bUserValue);
        }
        {
            // An owner that never called Unregister: the destructor restores too.
            FBackgroundThrottleGuard Guard;
            Guard.Register();
            Guard.Update(true, true);
        }
        TestEqual(FString::Printf(TEXT("user=%d: destructor restores the user's value"), bUserValue),
            ThrottleFlag(), bUserValue);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBackgroundThrottleRecentDispatchTest,
    "PinWright.state.background_throttle.RecentDispatchCountsAsActivity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackgroundThrottleRecentDispatchTest::RunTest(const FString& Parameters)
{
    // Only the ledger half of IsAgentActive is asserted: running jobs from other tests
    // may exist, so "no activity -> inactive" is not observable here.
    ClientActivity::ResetForTests();
    double SecondsAgo = 0.0;
    TestFalse(TEXT("empty ledger has no last dispatch"),
        ClientActivity::GetSecondsSinceLastDispatch(SecondsAgo));

    const double Now = FPlatformTime::Seconds();
    ClientActivity::NoteDispatchForTests(TEXT("agent-a"), TEXT("actor.list"), Now - 1000.0);
    ClientActivity::NoteDispatchForTests(FString(), TEXT("actor.describe"), Now - 10.0);
    TestTrue(TEXT("ledger has a last dispatch"), ClientActivity::GetSecondsSinceLastDispatch(SecondsAgo));
    TestTrue(TEXT("the most recent dispatch of any client wins, anonymous included"),
        SecondsAgo >= 10.0 && SecondsAgo < 100.0);
    TestTrue(TEXT("a dispatch 10 s ago keeps the agent active"), FBackgroundThrottleGuard::IsAgentActive());

    ClientActivity::ResetForTests();
    return true;
}
