// Copyright (c) 2026 Alexander Penkin. MIT License.

// Board B-screenshot-designer-hangs-game-thread: the two Designer retry loops were bounded by
// attempt count only, so slow Slate pumps could hold the game thread with no wall-clock limit.
// Each test sets the budget to 0 and drives a loop that can never succeed (no editor). With the
// budget check the loop answers TIMEOUT before its first pump; without it the loop runs all twelve
// attempts and returns its count-exhausted result (empty error / EDITOR_NOT_FOUND), which fails.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/UI/WidgetDesignerCaptureInternal.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // The resolve loop checks readiness before each attempt; an editor still loading cannot reach
    // the budget branch at all, so that host skips rather than failing on EDITOR_NOT_READY.
    bool SkipIfDesignerNotReady(FAutomationTestBase& Test)
    {
        FString ReadinessError;
        if (WidgetDesignerCaptureInternal::QueryDesignerCaptureReadiness(ReadinessError))
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(Test, TEXT("designer-not-ready"),
            FString::Printf(TEXT("QueryDesignerCaptureReadiness returned %s"), *ReadinessError));
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerRetryBudgetHostWindowTest,
    "PinWright.widget.screenshot_designer.RetryBudget.HostWindowLoopTimesOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerRetryBudgetHostWindowTest::RunTest(const FString& Parameters)
{
    using namespace WidgetDesignerCaptureInternal;

    const double SavedBudget = RetryBudgetSeconds();
    ON_SCOPE_EXIT { RetryBudgetSeconds() = SavedBudget; };
    RetryBudgetSeconds() = 0.0;

    FString Error;
    const TSharedPtr<SWindow> Window = FindWidgetEditorHostWindowWithRetry(nullptr, &Error);
    TestFalse(TEXT("no editor resolves no window"), Window.IsValid());
    TestEqual(TEXT("an exhausted budget answers TIMEOUT instead of pumping again"),
        Error, FString(ErrorCodes::ERR_TIMEOUT));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerRetryBudgetPreviewResolveTest,
    "PinWright.widget.screenshot_designer.RetryBudget.PreviewResolveLoopTimesOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerRetryBudgetPreviewResolveTest::RunTest(const FString& Parameters)
{
    using namespace WidgetDesignerCaptureInternal;

    if (SkipIfDesignerNotReady(*this)) { return true; }

    const double SavedBudget = RetryBudgetSeconds();
    ON_SCOPE_EXIT { RetryBudgetSeconds() = SavedBudget; };
    RetryBudgetSeconds() = 0.0;

    FWidgetDesignerPreviewTarget Target;
    FString Error;
    const bool bResolved = ResolveDesignerPreviewTargetWithRetry(
        nullptr, nullptr, Target, Error, /*bDesignerAlreadyOpen=*/true);
    TestFalse(TEXT("no editor resolves no preview"), bResolved);
    TestEqual(TEXT("an exhausted budget answers TIMEOUT instead of pumping again"),
        Error, FString(ErrorCodes::ERR_TIMEOUT));
    return true;
}
