// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"

#include "Decompiler/BpirDecompiler.h"
#include "Engine/Blueprint.h"
#include "K2Node_DynamicCast.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompilerCastUnknownDiagnosticTest,
    "PinWright.bpir.decompiler.CastUnknownDiagnostic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirDecompilerCastUnknownDiagnosticTest::RunTest(const FString& Parameters)
{
    // Counterfactual: without this fix, R.Warnings contains zero entries matching
    // the cast-diagnostic phrase even though R.BpirText already shows cast<Unknown>.
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);

    UK2Node_DynamicCast* CastNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_DynamicCast>(EventGraph);
    CastNode->TargetType = nullptr;
    CastNode->ReconstructNode();
    BpirGraphTestHelpers::WireExec(BeginPlayNode, CastNode);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("BPIR text contains cast<Unknown> visible signal"),
        Result.BpirText.Contains(TEXT("cast<Unknown>")));

    const FString CastGuid = CastNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
    const FString BpPath = BP->GetPathName();
    const FString GraphName = EventGraph->GetName();

    TArray<FString> Matching;
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        if (Warning.Text.Contains(CastGuid))
        {
            Matching.Add(Warning.Text);
        }
    }

    TestEqual(TEXT("Exactly one warning matching the cast node GUID"), Matching.Num(), 1);
    if (Matching.Num() != 1) { return false; }

    const FString& W = Matching[0];
    TestTrue(TEXT("Warning contains class name K2Node_DynamicCast"),
        W.Contains(TEXT("K2Node_DynamicCast")));
    TestTrue(TEXT("Warning contains graph name"),
        W.Contains(GraphName));
    TestTrue(TEXT("Warning contains Blueprint path"),
        W.Contains(BpPath));
    TestTrue(TEXT("Warning contains stable cast<Unknown> phrase"),
        W.Contains(TEXT("cast<Unknown>")) || W.Contains(TEXT("no resolved target type")));
    return true;
}
