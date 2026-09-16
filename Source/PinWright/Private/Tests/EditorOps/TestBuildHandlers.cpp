// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Build domain handlers (PipelineHandler.cpp)
// Handlers covered: pipeline.get_status, plus the shared UBT entry-point
// resolver (Handlers/BuildTools/UbtEntryPoint.h) that system.run_ubt calls.
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/BuildTools/UbtEntryPoint.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

// ============================================================================
// Regression for B-pipeline-run-ubt-bad-exe-path: the UBT entry point was once
// hardcoded to Engine/Build/BatchFiles/RunUBT.bat, which does NOT exist in the
// Windows engine layout (only RunUBT.sh ships, for Mac/Linux). CreateProc was
// then handed a path to a missing file and every Windows call died
// CREATEPROC_FAILED before any build could run. The fix resolves the real
// cross-platform wrapper (Build.bat on Windows / Build.sh elsewhere). This test
// calls the SAME production resolver system.run_ubt uses and would fail if the
// fix were reverted to the dead RunUBT.bat path. It never spawns a build.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildRunUbtEntryPointExistsTest,
    "PinWright.system.run_ubt.EntryPointResolvesToExistingScript",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBuildRunUbtEntryPointExistsTest::RunTest(const FString& Parameters)
{
    const FString UbtExe = EARG_Ubt::ResolveUbtEntryPoint();

    // The resolved wrapper must be the real per-platform forwarding script
    // (Build.bat / Build.sh), never the nonexistent RunUBT.bat the bug used —
    // the positive basename check below already excludes that dead path.
#if PLATFORM_WINDOWS
    TestTrue(TEXT("Windows UBT entry point is Build.bat"),
        UbtExe.EndsWith(TEXT("Build.bat")));
#else
    TestTrue(TEXT("Non-Windows UBT entry point is Build.sh"),
        UbtExe.EndsWith(TEXT("Build.sh")));
#endif

    // The selected wrapper must actually exist in this engine install. Under the
    // old hardcoded RunUBT.bat this assertion fails on Windows (file is absent),
    // which is precisely the CREATEPROC_FAILED root cause the fix removes.
    TestTrue(FString::Printf(TEXT("UBT entry point exists on disk: %s"), *UbtExe),
        FPaths::FileExists(UbtExe));
    return true;
}

// ============================================================================
// pipeline.get_status — RPC_NO_PARAMS; must read engine version and project
// info from UE globals without crashing in the test environment.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildGetStatusNoCrashTest,
    "PinWright.pipeline.get_status.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBuildGetStatusNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("pipeline.get_status found and invoked"),
        InvokeHandler(TEXT("pipeline.get_status"), Payload));
    return true;
}
