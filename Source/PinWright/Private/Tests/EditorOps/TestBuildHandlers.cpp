// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Build domain handlers (PipelineHandler.cpp)
// Handlers covered: pipeline.get_status.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

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
