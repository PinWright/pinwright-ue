// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR template / dispatch round-trip regressions. Three tests cover the
// three classifier arms introduced by F-crir-template-and-dispatch:
//   1. Bare URigVMTemplateNode  -> `template <Notation> (...)` opcode
//   2. URigVMDispatchNode (Print factory)     -> `dispatch RigVMDispatch_Print (...)`
//   3. URigVMDispatchNode (ArrayAdd factory)  -> `dispatch RigVMDispatch_ArrayAdd (...)`
//
// Test #2 originally used URigVMController::AddBranchNode, but that method
// returns a URigVMUnitNode wrapping FRigVMFunction_ControlFlowBranch (verified
// against UE 5.6 Engine source, RigVMController.cpp AddBranchNode at L16418):
// there is no `RigVMDispatch_Branch` factory in the engine. Print is the
// minimal real dispatch factory that exercises the general (non-If/Select)
// dispatch arm in CRIRDecompiler.cpp without overlapping with the ArrayAdd
// coverage in test #3.
//
// Per-test counterfactual comments live next to each test below.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RigVMCore/RigVMByteCode.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"

// Counterfactual: if the bare-template arm added after the unit arm in
// CRIRDecompiler.cpp is reverted, this template falls into the default TODO
// arm and emits as `# TODO unsupported node kind: RigVMTemplateNode`. Result2
// (compile-back of the TODO comment) loses the template node entirely,
// breaking byte-equality with Result1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRTemplate_RoundTrip,
    "PinWright.CRIR.RoundTrip.Template",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRTemplate_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRTmpl_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRTmpl_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRTmpl_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRTmpl_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR template: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRTmpl_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR template: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    // Use the Add dispatch's wildcard template notation. AddBranchNode would
    // resolve to a concrete dispatch (control flow); the Add factory's
    // wildcard form stays a bare URigVMTemplateNode until a type is wired in.
    URigVMNode* TmplNode = SourceController->AddTemplateNode(
        FName(TEXT("Add::Execute(in A,in B,out Result)")),
        FVector2D(0, 0),
        FString(TEXT("WildcardAdd")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!TmplNode)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("node-construction-failed"),
            TEXT("CRIR template: AddTemplateNode returned null for wildcard Add notation - skipped."));
        return true;
    }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile template (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("template round-trip text equality"), Normalized2, Normalized1);
    TestTrue(TEXT("template opcode present in decompile #1"), Normalized1.Contains(TEXT("= template ")));
    return true;
}

// Counterfactual: if the general dispatch arm fallthrough in
// CRIRDecompiler.cpp (inserted after the if/select special case) is reverted,
// the Print node lands in the default TODO arm and Result2 has no print
// node — breaking byte-equality and dropping the `dispatch RigVMDispatch_Print`
// line entirely.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRDispatch_Print_RoundTrip,
    "PinWright.CRIR.RoundTrip.DispatchPrint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRDispatch_Print_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRDispPr_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRDispPr_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRDispPr_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRDispPr_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR dispatch print: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRDispPr_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR dispatch print: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    // Construct a real URigVMDispatchNode via the registry+template-notation
    // path the compiler itself uses (CRIRCompiler.cpp around L523-L540). This
    // exercises the general-dispatch decompile arm rather than the If/Select
    // special-case arm.
    FRigVMRegistry& Registry = FRigVMRegistry::Get();
    const FRigVMDispatchFactory* PrintFactory =
        Registry.FindDispatchFactory(FName(TEXT("DISPATCH_RigVMDispatch_Print")));
    TestNotNull(TEXT("RigVMDispatch_Print factory registered"), PrintFactory);
    if (!PrintFactory) { return false; }

    URigVMNode* PrintNode = SourceController->AddTemplateNode(
        PrintFactory->GetTemplateNotation(),
        FVector2D::ZeroVector,
        FString(TEXT("PrintA")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("print dispatch created"), PrintNode);
    if (!PrintNode) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile dispatch print (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("dispatch print round-trip text equality"), Normalized2, Normalized1);
    TestTrue(TEXT("dispatch RigVMDispatch_Print present"),
        Normalized1.Contains(TEXT("dispatch RigVMDispatch_Print")));
    return true;
}

// Counterfactual: same root cause as branch test (reverting the general
// dispatch arm loses the array node), plus this test proves the unified
// compile path (`AddTemplateNode(Factory->GetTemplateNotation())`) works for
// the array family without per-factory sugar (`AddArrayNode`).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRDispatch_ArrayAdd_RoundTrip,
    "PinWright.CRIR.RoundTrip.DispatchArrayAdd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRDispatch_ArrayAdd_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRDispArr_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRDispArr_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRDispArr_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRDispArr_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR dispatch array-add: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRDispArr_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR dispatch array-add: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* ArrayAddNode = SourceController->AddArrayNode(
        ERigVMOpCode::ArrayAdd,
        TEXT("float"),
        /*InCPPTypeObject*/ nullptr,
        FVector2D::ZeroVector,
        FString(TEXT("ArrAdd")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("array-add created"), ArrayAddNode);
    if (!ArrayAddNode) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile dispatch array-add (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("dispatch array-add round-trip text equality"), Normalized2, Normalized1);
    TestTrue(TEXT("dispatch line present in decompile #1"),
        Normalized1.Contains(TEXT("= dispatch ")));
    return true;
}
