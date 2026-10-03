// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirWarnMarkerColumn.cpp - B-bpir-warn-marker-not-newline-separated. bpir.txt wrote
// the graph body's closing brace and the first warning marker on one line
// (`}# BPIR_WARN: ...`), hiding both from anchored line consumers. Checks the whole class:
// no `# BPIR_` marker may start past column 0.

#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"

#include "Utils/AssetDumpBuilder.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirWarnMarkerColumnTest,
    "PinWright.bpir.warn_marker_column.MarkersStartAtColumnZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirWarnMarkerColumnTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!TestNotNull(TEXT("Blueprint was created"), BP) || !TestTrue(TEXT("Has an event graph"), BP->UbergraphPages.Num() > 0))
    {
        return false;
    }

    // A reachable body (so the graph text ends in `}`) plus an unwired impure call (so the
    // graph carries an orphan warning).
    UEdGraph* EventGraph = BP->UbergraphPages[0];
    BpirGraphTestHelpers::WireExec(
        BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph),
        BpirGraphTestHelpers::AddPrintStringNode(EventGraph));
    BpirGraphTestHelpers::AddPrintStringNode(EventGraph);

    const FString Output = AssetDumpBuilder::BuildBpirText(BP);
    if (!TestTrue(FString::Printf(TEXT("Fixture produces an orphan warning marker (text:\n%s)"), *Output),
            Output.Contains(TEXT("BPIR_WARN: Orphaned node"))))
    {
        return false;
    }

    TArray<FString> Lines;
    Output.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const int32 Column = Lines[Index].Find(TEXT("# BPIR_"));
        TestTrue(FString::Printf(TEXT("Line %d starts its marker at column 0: '%s'"), Index + 1, *Lines[Index]),
            Column == INDEX_NONE || Column == 0);
    }
    return true;
}
