// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction guard for `geometry.bridge`'s declared parameter surface.
//
// `subdivisions` was once declared, read and echoed while never reaching the geometry:
// bridge is hand-rolled over FDynamicMesh3::AppendTriangle and emits exactly one triangle
// strip between the two loops, so no value could change the output. The repair was to REJECT
// rather than implement - the knob is off the declared surface, which puts it behind the
// dispatcher's UNKNOWN_PARAMS gate (RpcDispatcher.cpp), so a caller who passes it is told
// immediately instead of reading an affirmative echo.
//
// Nothing else fails if the knob comes back: re-declaring it would compile, register and pass
// every existing bridge test, because those assert the handler runs, not what it accepts. This
// test is the only thing that fails in that direction.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBridgeSubdivisionsNotDeclaredTest,
    "PinWright.geometry.bridge.SubdivisionsStaysOffTheDeclaredSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBridgeSubdivisionsNotDeclaredTest::RunTest(const FString& Parameters)
{
    // Control: a parameter bridge really does honour, so a bug that emptied the whole spec
    // could not make the assertion below pass vacuously.
    TestNotNull(TEXT("geometry.bridge still declares edgeGroupA"),
        GetRegisteredParamSpec(TEXT("geometry.bridge"), TEXT("edgeGroupA")));

    TestNull(TEXT("geometry.bridge must not declare `subdivisions` - it cannot apply one, so "
                  "declaring it would re-open the accepted-and-ignored path"),
        GetRegisteredParamSpec(TEXT("geometry.bridge"), TEXT("subdivisions")));
    return true;
}
