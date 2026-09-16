// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for NiagaraSearchHandler helpers and registered RPCs.
// Covers: PayloadKindFor, BuildOpSignature, ScoreOpMatch (via NiagaraSearchHandler.h),
// and dispatcher registration of niagara.graph.list_node_types / niagara.graph.search_ops.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraSearchHandler.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOp.h"
#include "NiagaraEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchHandlerTest,
    "PinWright.niagara.search.GraphNodeTypesAndOps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchHandlerTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraSearch;

    // ------------------------------------------------------------------
    // PayloadKindFor tests
    // ------------------------------------------------------------------

    TestEqual(TEXT("Op kind"),
        PayloadKindFor(UNiagaraNodeOp::StaticClass()),
        FString(TEXT("op_name")));

    TestEqual(TEXT("FunctionCall kind"),
        PayloadKindFor(UNiagaraNodeFunctionCall::StaticClass()),
        FString(TEXT("script_asset")));

    // UNiagaraNode itself is abstract — falls through to default "none"
    TestEqual(TEXT("Abstract base kind"),
        PayloadKindFor(UNiagaraNode::StaticClass()),
        FString(TEXT("none")));

    TestEqual(TEXT("Null class kind"),
        PayloadKindFor(nullptr),
        FString(TEXT("none")));

    // ------------------------------------------------------------------
    // ScoreOpMatch tests
    // ------------------------------------------------------------------

    // Exact match on name → score >= 1000
    TestTrue(TEXT("Exact 'Add' match scores >= 1000"),
        ScoreOpMatch(TEXT("Add"), TEXT("Add"), TEXT(""), TEXT(""), TEXT("")) >= 1000);

    // Completely different name → 0
    TestEqual(TEXT("'Add' vs 'Multiply' scores 0"),
        ScoreOpMatch(TEXT("Add"), TEXT("Multiply"), TEXT(""), TEXT(""), TEXT("")),
        0);

    // Garbage query → 0
    TestEqual(TEXT("Garbage query 'zzz' scores 0"),
        ScoreOpMatch(TEXT("zzz"), TEXT("Add"), TEXT(""), TEXT(""), TEXT("")),
        0);

    // Empty query → 0 (not a match, caller skips scoring)
    TestEqual(TEXT("Empty query scores 0"),
        ScoreOpMatch(TEXT(""), TEXT("Add"), TEXT(""), TEXT(""), TEXT("")),
        0);

    // Prefix match
    TestTrue(TEXT("'Ad' prefix of 'Add' scores >= 500"),
        ScoreOpMatch(TEXT("Ad"), TEXT("Add"), TEXT(""), TEXT(""), TEXT("")) >= 500);

    // ------------------------------------------------------------------
    // Dispatcher registration
    // ------------------------------------------------------------------

    TestTrue(TEXT("niagara.graph.list_node_types is registered"),
        IsRegistered(TEXT("niagara.graph.list_node_types")));

    TestTrue(TEXT("niagara.graph.search_ops is registered"),
        IsRegistered(TEXT("niagara.graph.search_ops")));

    // ------------------------------------------------------------------
    // NiagaraGraphListNodeTypes_ReturnsCorePayloadKinds
    // ------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("niagara.graph.list_node_types"), nullptr, Capture);
        TestTrue(TEXT("list_node_types handler found"), bFound);
        TestTrue(TEXT("list_node_types sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("list_node_types succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* NodeTypesArr = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("nodeTypes"), NodeTypesArr) && NodeTypesArr)
            {
                TestTrue(TEXT("list_node_types returns >5 entries"),
                    NodeTypesArr->Num() > 5);

                bool bFoundFunctionCall = false;
                bool bFoundNodeOp = false;
                for (const TSharedPtr<FJsonValue>& Entry : *NodeTypesArr)
                {
                    const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
                    if (!Entry->TryGetObject(ObjPtr) || !ObjPtr) continue;

                    FString ClassName;
                    FString PayloadKind;
                    (*ObjPtr)->TryGetStringField(TEXT("className"), ClassName);
                    (*ObjPtr)->TryGetStringField(TEXT("payloadKind"), PayloadKind);

                    if (ClassName == TEXT("NiagaraNodeFunctionCall") && PayloadKind == TEXT("script_asset"))
                    {
                        bFoundFunctionCall = true;
                    }
                    if (ClassName == TEXT("NiagaraNodeOp") && PayloadKind == TEXT("op_name"))
                    {
                        bFoundNodeOp = true;
                    }
                }
                TestTrue(TEXT("list_node_types has NiagaraNodeFunctionCall with payloadKind=script_asset"),
                    bFoundFunctionCall);
                TestTrue(TEXT("list_node_types has NiagaraNodeOp with payloadKind=op_name"),
                    bFoundNodeOp);
            }
            else
            {
                AddError(TEXT("list_node_types result missing 'nodeTypes' array"));
            }
        }
    }

    // ------------------------------------------------------------------
    // NiagaraGraphSearchOps_FindsAdd
    // ------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("query"), TEXT("Add"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.graph.search_ops"), Params, Capture);
        TestTrue(TEXT("search_ops 'Add' sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("search_ops 'Add' succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* ResultsArr = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("results"), ResultsArr) && ResultsArr)
            {
                TestTrue(TEXT("search_ops 'Add' returns >0 results"),
                    ResultsArr->Num() > 0);

                if (ResultsArr->Num() > 0)
                {
                    const TSharedPtr<FJsonObject>* TopObj = nullptr;
                    if ((*ResultsArr)[0]->TryGetObject(TopObj) && TopObj)
                    {
                        FString OpName;
                        double Score = 0.0;
                        (*TopObj)->TryGetStringField(TEXT("opName"), OpName);
                        (*TopObj)->TryGetNumberField(TEXT("score"), Score);

                        TestTrue(TEXT("search_ops 'Add' top result opName contains 'add' (case-insensitive)"),
                            OpName.ToLower().Contains(TEXT("add")));
                        TestTrue(TEXT("search_ops 'Add' top result score > 0"),
                            Score > 0.0);
                    }
                }
            }
            else
            {
                AddError(TEXT("search_ops 'Add' result missing 'results' array"));
            }
        }
    }

    // ------------------------------------------------------------------
    // NiagaraGraphSearchOps_EmptyQueryReturnsAll
    // ------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // empty query, default limit=50

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.graph.search_ops"), Params, Capture);
        TestTrue(TEXT("search_ops empty query sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("search_ops empty query succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* ResultsArr = nullptr;
            double TotalMatchesVal = 0.0;
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatchesVal);

            if (Capture.Result->TryGetArrayField(TEXT("results"), ResultsArr) && ResultsArr)
            {
                // Default limit is 50; results should be capped at 50
                TestTrue(TEXT("search_ops empty query results <= 50"),
                    ResultsArr->Num() <= 50);
                // But the full op registry has more than 50 ops
                TestTrue(TEXT("search_ops empty query totalMatches > 50"),
                    static_cast<int32>(TotalMatchesVal) > 50);
            }
            else
            {
                AddError(TEXT("search_ops empty query result missing 'results' array"));
            }
        }
    }

    // ------------------------------------------------------------------
    // NiagaraGraphSearchOps_GarbageQueryReturnsEmpty
    // ------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("query"), TEXT("zzzzzzz_no_such_op"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.graph.search_ops"), Params, Capture);
        TestTrue(TEXT("search_ops garbage query sent a response"), Capture.bWasCalled);
        TestTrue(TEXT("search_ops garbage query succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* ResultsArr = nullptr;
            double TotalMatchesVal = -1.0;
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatchesVal);

            Capture.Result->TryGetArrayField(TEXT("results"), ResultsArr);
            const int32 ResultCount = ResultsArr ? ResultsArr->Num() : 0;

            TestEqual(TEXT("search_ops garbage query returns 0 results"), ResultCount, 0);
            TestEqual(TEXT("search_ops garbage query totalMatches == 0"),
                static_cast<int32>(TotalMatchesVal), 0);
        }
    }


    return true;
}
