// Copyright (c) 2026 Alexander Penkin. MIT License.

// E-setup-window-opens-on-every-agent-start: the PinWright Setup screen opened at every editor
// start (bShowSetupScreenOnLaunch defaults true), also in -unattended automation runs, where on a
// real display its window covered the level-viewport PIE the drive tests click into. The
// launch-time open is now skipped under -unattended. The suite always runs -unattended
// (pinwright_supervisor.suite_argv), so the tab must not be live here; nothing in the suite opens it.
#include "Misc/AutomationTest.h"

#include "PinWrightSettings.h"
#include "Setup/SGatewaySetupScreen.h"
#include "Tests/TestSkipReporting.h"

#include "Framework/Docking/TabManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetupScreenNotOpenedUnattendedTest,
    "PinWright.infra.setup_screen.NotOpenedUnattended",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSetupScreenNotOpenedUnattendedTest::RunTest(const FString& Parameters)
{
    // The switch itself: FApp::IsUnattended() is also true whenever GIsAutomationTesting is,
    // i.e. throughout this test, though the module read it at launch when it was not.
    if (!FParse::Param(FCommandLine::Get(), TEXT("unattended")))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_not_unattended"),
            TEXT("Only an -unattended editor suppresses the launch-time Setup screen."));
        return true;
    }
    // Without the launch setting nothing would open even with the gate reverted.
    if (!GetDefault<UPinWrightSettings>()->bShowSetupScreenOnLaunch)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("setup_screen_launch_disabled"),
            TEXT("bShowSetupScreenOnLaunch is off on this host, so the launch gate is not exercised."));
        return true;
    }
    TestFalse(TEXT("an -unattended editor did not open the PinWright Setup screen at launch"),
        FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(SGatewaySetupScreen::GetTabId())).IsValid());
    return true;
}
