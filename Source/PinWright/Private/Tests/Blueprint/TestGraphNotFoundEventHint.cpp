// Copyright (c) 2026 Alexander Penkin. MIT License.

// Guards the GRAPH_NOT_FOUND message enrichment shared by blueprint.graph.* (via the
// ResolveBlueprintAndGraph resolver) and blueprint.references. Both used to emit a bare
// "Could not find graph 'X' in blueprint." that named neither the available graphs nor
// the fact that an event name (e.g. the `entry override Tick(...)` token from
// blueprint.decompile) is not a graph. BuildGraphNotFoundMessage now enumerates the
// blueprint's graphs and hints that the name may be an event inside EventGraph.
// Exercises the production helper directly (pure, no live blueprint needed); reverting
// the enrichment fails these assertions.

#include "Misc/AutomationTest.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphNotFoundMessageEnumeratesAndHints,
    "PinWright.Blueprint.GraphNotFound.EnumeratesAndHints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphNotFoundMessageEnumeratesAndHints::RunTest(const FString& Parameters)
{
    // The reporter's exact case: passing the decompiled event name "Tick" as graphName.
    const TArray<FString> Available = { TEXT("EventGraph"), TEXT("MyFunction") };
    const FString Message =
        BlueprintGraphHelpers::BuildGraphNotFoundMessage(TEXT("Tick"), Available);

    TestTrue(TEXT("retains the original not-found phrasing"),
        Message.Contains(TEXT("Could not find graph 'Tick' in blueprint.")));
    TestTrue(TEXT("enumerates the available graph names"),
        Message.Contains(TEXT("Available graphs:")) &&
            Message.Contains(TEXT("EventGraph")) && Message.Contains(TEXT("MyFunction")));
    TestTrue(TEXT("hints that an event name lives inside EventGraph"),
        Message.Contains(TEXT("event")) && Message.Contains(TEXT("EventGraph")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphNotFoundMessageHandlesNoGraphs,
    "PinWright.Blueprint.GraphNotFound.HandlesNoGraphs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphNotFoundMessageHandlesNoGraphs::RunTest(const FString& Parameters)
{
    // With no available graphs the enumeration clause is omitted, but the event hint
    // (the load-bearing half of the fix) must still be present.
    // Explicitly typed empty list: a bare {} is ambiguous between the FString-name and
    // UEdGraph* overloads. This test exercises the pure name-based overload.
    const FString Message =
        BlueprintGraphHelpers::BuildGraphNotFoundMessage(TEXT("Tick"), TArray<FString>{});

    TestTrue(TEXT("retains the original not-found phrasing"),
        Message.Contains(TEXT("Could not find graph 'Tick' in blueprint.")));
    TestFalse(TEXT("omits the available-graphs clause when there are none"),
        Message.Contains(TEXT("Available graphs:")));
    TestTrue(TEXT("still emits the EventGraph event hint"),
        Message.Contains(TEXT("EventGraph")));
    return true;
}
