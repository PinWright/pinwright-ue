// Copyright (c) 2026 Alexander Penkin. MIT License.

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/UObjectGlobals.h"

#include "PCGGraph.h"

// The Procedural Vegetation Editor is the reference case for an optional-plugin dependency
// carried by a PARAMETER VALUE rather than by a namespace or a method prefix: pcg.add_node
// works without it and needs it only when `nodeClass` names a /Script/ProceduralVegetation
// class. It ships with every stock UE 5.8 but is experimental and "EnabledByDefault": false,
// which is exactly why the tree's __has_include pattern cannot guard this path — a build-time
// probe answers "present" on the hosts where the module is never loaded.
//
// This test pins the runtime guard's contract: a typed refusal that NAMES the plugin, never a
// bare CLASS_NOT_FOUND (which reads as "you mistyped a path" for a path that is correct on any
// host with the plugin on) and never a silent success. On a host where the plugin is absent or
// enabled the branch is unreachable, so the test SKIPS with the wire marker instead of passing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGAddNodeNamesDisabledPluginTest,
    "PinWright.pcg.add_node.NamesDisabledPlugin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGAddNodeNamesDisabledPluginTest::RunTest(const FString& Parameters)
{
    const TCHAR* const PluginName = TEXT("ProceduralVegetationEditor");
    const TCHAR* const NodeClassPath = TEXT("/Script/ProceduralVegetation.PVBaseSettings");

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
    if (!Plugin.IsValid())
    {
        // Engines that predate the plugin, and stripped engines, have no disabled-plugin state
        // to observe. Reported as a skip, not counted as a pass.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
            FString::Printf(TEXT("this engine does not ship the '%s' plugin"), PluginName));
        return true;
    }

    if (Plugin->IsEnabled())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-enabled"),
            FString::Printf(
                TEXT("'%s' is enabled in this project, so the disabled-plugin branch is unreachable here"),
                PluginName));
        return true;
    }

    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Payload->SetStringField(TEXT("nodeClass"), NodeClassPath);
    Payload->SetNumberField(TEXT("x"), 0);
    Payload->SetNumberField(TEXT("y"), 0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("pcg.add_node"), Payload, Capture));
    TestTrue(TEXT("responded"), Capture.bWasCalled);
    TestFalse(TEXT("refuses rather than reporting a node it did not add"), Capture.bSuccess);

    // Counterfactual: with the guard removed this path answers CLASS_NOT_FOUND
    // ("Could not resolve UPCGSettings subclass: ..."), and both assertions below fail.
    TestEqual(TEXT("error code is PLUGIN_DISABLED, not CLASS_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("PLUGIN_DISABLED")));
    TestTrue(TEXT("message names the plugin the caller must enable"),
        Capture.Message.Contains(PluginName));

    // The graph must be left untouched: a refusal that half-applied would be worse than
    // the bare CLASS_NOT_FOUND it replaces.
    TestEqual(TEXT("no node was appended"), Graph->GetNodes().Num(), 0);
    return true;
}

#endif // __has_include("PCGGraph.h")
