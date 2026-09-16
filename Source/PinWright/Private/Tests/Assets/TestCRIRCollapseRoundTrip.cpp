// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR collapse round-trip regression. Builds a Control Rig with unit nodes,
// wraps them via URigVMController::CollapseNodes, decompiles to CRIR, compiles
// into a fresh target asset, decompiles again, and asserts byte-equal text.
//
// Two cases:
// - Single-level collapse (wraps three unit nodes).
//   Counterfactual: If `EmitGraphBody` doesn't recurse into
//   `Collapse->GetContainedGraph()`, the collapse node falls into the default
//   TODO arm; Result2 has no collapse and byte-equality fails.
// - Nested two-level collapse (collapse of [collapse + two units]).
//   Counterfactual: If the parser brace-stack tops out at depth 1, the inner
//   `rig_subgraph` line emits `CRIR_BAD_BLOCK_HEADER` on Result2's compile and
//   Result2 is empty - byte-equality fails.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMDefines.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Units/RigUnit.h"
#include "Utils/AssetUtils.h"

namespace
{
URigVMNode* AddIdentityFloat(URigVMController* Controller, const FString& NodeName, const FVector2D& Position)
{
    // RigVMFunction_MathIntAdd is a plain unit op (not an event), so multiple
    // instances can coexist in the same graph — RigUnit_BeginExecution would
    // collide on the second instantiation ("Event ... already exists").
    return Controller->AddUnitNodeFromStructPath(
        TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"),
        FRigUnit::GetMethodName(),
        Position,
        NodeName,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCollapse_SingleLevel_RoundTrip,
    "PinWright.CRIR.RoundTrip.CollapseSingleLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRCollapse_SingleLevel_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRColl1_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRColl1_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRColl1_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRColl1_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR collapse single-level: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRColl1_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR collapse single-level: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* A = AddIdentityFloat(SourceController, TEXT("NodeA"), FVector2D(0, 0));
    URigVMNode* B = AddIdentityFloat(SourceController, TEXT("NodeB"), FVector2D(200, 0));
    URigVMNode* C = AddIdentityFloat(SourceController, TEXT("NodeC"), FVector2D(400, 0));
    TestNotNull(TEXT("NodeA"), A);
    TestNotNull(TEXT("NodeB"), B);
    TestNotNull(TEXT("NodeC"), C);
    if (!A || !B || !C) { return false; }

    TArray<FName> Names = { A->GetFName(), B->GetFName(), C->GetFName() };
    URigVMCollapseNode* CollapseNode = SourceController->CollapseNodes(
        Names,
        FString(TEXT("CollapseA")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false,
        /*bIsAggregate*/ false);
    TestNotNull(TEXT("collapse node created"), CollapseNode);
    if (!CollapseNode) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile collapse single-level (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("collapse single-level round-trip text equality"), Normalized2, Normalized1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCollapse_NestedTwoLevel_RoundTrip,
    "PinWright.CRIR.RoundTrip.CollapseNested",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRCollapse_NestedTwoLevel_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRColl2_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRColl2_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRColl2_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRColl2_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR collapse nested: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRColl2_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR collapse nested: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* A = AddIdentityFloat(SourceController, TEXT("NodeA"), FVector2D(0, 0));
    URigVMNode* B = AddIdentityFloat(SourceController, TEXT("NodeB"), FVector2D(200, 0));
    URigVMNode* C = AddIdentityFloat(SourceController, TEXT("NodeC"), FVector2D(400, 0));
    if (!A || !B || !C) { return false; }

    TArray<FName> InnerNames = { A->GetFName(), B->GetFName(), C->GetFName() };
    URigVMCollapseNode* Inner = SourceController->CollapseNodes(
        InnerNames, FString(TEXT("CollapseInner")), false, false, false);
    TestNotNull(TEXT("inner collapse"), Inner);
    if (!Inner) { return false; }

    URigVMNode* D = AddIdentityFloat(SourceController, TEXT("NodeD"), FVector2D(600, 0));
    URigVMNode* E = AddIdentityFloat(SourceController, TEXT("NodeE"), FVector2D(800, 0));
    if (!D || !E) { return false; }

    TArray<FName> OuterNames = { Inner->GetFName(), D->GetFName(), E->GetFName() };
    URigVMCollapseNode* Outer = SourceController->CollapseNodes(
        OuterNames, FString(TEXT("CollapseOuter")), false, false, false);
    TestNotNull(TEXT("outer collapse"), Outer);
    if (!Outer) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile collapse nested (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("collapse nested round-trip text equality"), Normalized2, Normalized1);
    return true;
}

// Counterfactual: Before this fix the decompiler emits `# TODO unsupported node kind: RigVMAggregateNode` and the recompile produces an empty collapse stub or skips the node entirely; both the no-CRIR_UNSUPPORTED_NODE assertion and the rig_subgraph-present assertion would fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRDecompiler_AggregateNode_RoundTripsAsCollapse_DegradedClass,
    "PinWright.CRIR.RoundTrip.AggregateAsCollapse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRDecompiler_AggregateNode_RoundTripsAsCollapse_DegradedClass::RunTest(const FString& Parameters)
{
#if UE_RIGVM_AGGREGATE_NODES_ENABLED
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRAgg_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRAgg_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRAgg_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRAgg_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR aggregate-as-collapse: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRAgg_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR aggregate-as-collapse: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    // IntAdd is the canonical commutative binary op that AddAggregatePin can promote.
    URigVMNode* IntAddNode = SourceController->AddUnitNodeFromStructPath(
        TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"),
        FRigUnit::GetMethodName(),
        FVector2D(0, 0),
        TEXT("IntAddAgg"),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("IntAdd unit node"), IntAddNode);
    if (!IntAddNode) { return false; }

    // Two AddAggregatePin calls promote the unit node into a 3-input aggregate.
    // Empty pin name lets the controller auto-name; empty default keeps zero.
    const FString AggPin1 = SourceController->AddAggregatePin(
        IntAddNode->GetName(), FString(), FString(),
        /*bSetupUndoRedo*/ false, /*bPrintPythonCommand*/ false);
    TestFalse(TEXT("AddAggregatePin #1 returned non-empty pin path"), AggPin1.IsEmpty());
    const FString AggPin2 = SourceController->AddAggregatePin(
        IntAddNode->GetName(), FString(), FString(),
        /*bSetupUndoRedo*/ false, /*bPrintPythonCommand*/ false);
    TestFalse(TEXT("AddAggregatePin #2 returned non-empty pin path"), AggPin2.IsEmpty());

    // Pre-condition: source graph must contain exactly one URigVMAggregateNode.
    int32 AggregateCount = 0;
    for (URigVMNode* Node : SourceModel->GetNodes())
    {
        if (Cast<URigVMAggregateNode>(Node) != nullptr)
        {
            ++AggregateCount;
        }
    }
    TestEqual(TEXT("source graph aggregate-node count"), AggregateCount, 1);
    if (AggregateCount != 1) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile aggregate (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // No warning may contain CRIR_UNSUPPORTED_NODE — that would mean the
    // aggregate fell through to the default TODO arm.
    for (const FString& Warning : Result1.Warnings)
    {
        TestFalse(FString::Printf(TEXT("decompile warning lacks CRIR_UNSUPPORTED_NODE (was: '%s')"), *Warning),
            Warning.Contains(TEXT("CRIR_UNSUPPORTED_NODE")));
    }

    // Emitted text must include a collapse instruction (proof the collapse arm
    // caught the aggregate) and must not include the TODO sentinel string. The
    // decompiler uses the instruction form `%nN = collapse "Name" @(x,y) {`
    // rather than the sibling-block `rig_subgraph "Name" {` form so the
    // wrapper can carry a LocalId for wires and an authored position.
    TestTrue(TEXT("emitted text contains collapse instruction header"),
        Result1.CRIRText.Contains(TEXT("= collapse ")));
    TestFalse(TEXT("emitted text omits TODO unsupported aggregate sentinel"),
        Result1.CRIRText.Contains(TEXT("# TODO unsupported node kind: RigVMAggregateNode")));

    // Recompile and walk the target graph.
    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile aggregate-as-collapse (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    URigVMGraph* TargetModel = nullptr;
    URigVMController* TargetController = GetFirstModelController(TargetBP, TargetModel);
    TestNotNull(TEXT("target controller"), TargetController);
    if (!TargetController || !TargetModel) { return false; }

    // Find at least one collapse-shaped node. Do NOT require URigVMAggregateNode
    // — aggregate class identity is documented-loss per ticket F-crir-aggregate-node.
    URigVMCollapseNode* RecompiledCollapse = nullptr;
    for (URigVMNode* Node : TargetModel->GetNodes())
    {
        if (URigVMCollapseNode* Collapse = Cast<URigVMCollapseNode>(Node))
        {
            RecompiledCollapse = Collapse;
            break;
        }
    }
    TestNotNull(TEXT("recompiled graph contains a collapse-shaped node"), RecompiledCollapse);
    if (!RecompiledCollapse) { return false; }

    URigVMGraph* ContainedGraph = RecompiledCollapse->GetContainedGraph();
    TestNotNull(TEXT("recompiled collapse has a contained graph"), ContainedGraph);
    if (!ContainedGraph) { return false; }

    bool bFoundInnerUnit = false;
    for (URigVMNode* InnerNode : ContainedGraph->GetNodes())
    {
        if (Cast<URigVMUnitNode>(InnerNode) != nullptr)
        {
            bFoundInnerUnit = true;
            break;
        }
    }
    TestTrue(TEXT("contained graph carries at least one unit node (inner IntAdd survives)"), bFoundInnerUnit);
    return true;
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("aggregate-nodes-disabled"),
        TEXT("UE_RIGVM_AGGREGATE_NODES_ENABLED is 0 in this engine build; skipping aggregate round-trip test."));
    return true;
#endif
}
