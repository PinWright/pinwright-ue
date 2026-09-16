// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR invoke_entry round-trip regression. A BeginExecution event entry plus
// an InvokeEntry node referencing a named sub-routine are added, wired through
// the ExecuteContext pins, and the source/target CRIR texts must round-trip
// byte-equal.
//
// Counterfactual: if the compiler's ECRIROpcode::InvokeEntry case is removed,
// the compile-back falls through to the default arm and returns
// CRIR_UNSUPPORTED_OPCODE — the bSuccess assertion fails.
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
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Units/RigUnit.h"
#include "Utils/AssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRInvokeEntry_RoundTrip,
    "PinWright.CRIR.RoundTrip.InvokeEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRInvokeEntry_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRInv_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRInv_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRInv_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRInv_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR invoke_entry: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRInv_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR invoke_entry: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* BeginNode = SourceController->AddUnitNodeFromStructPath(
        TEXT("/Script/ControlRig.RigUnit_BeginExecution"),
        FRigUnit::GetMethodName(),
        FVector2D(0, 0),
        FString(TEXT("BeginExecution")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("BeginExecution created"), BeginNode);

    URigVMNode* InvokeNode = SourceController->AddInvokeEntryNode(
        FName(TEXT("SubRoutine")),
        FVector2D(300, 0),
        FString(TEXT("InvokeSub")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("invoke entry created"), InvokeNode);
    if (!BeginNode || !InvokeNode) { return false; }

    const FString BeginPin = FString::Printf(TEXT("%s.ExecuteContext"), *BeginNode->GetName());
    const FString InvokePin = FString::Printf(TEXT("%s.ExecuteContext"), *InvokeNode->GetName());
    const bool bWired = SourceController->AddLink(
        BeginPin, InvokePin,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("wired BeginExecution -> InvokeEntry exec context"), bWired);

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Independent reference. Byte-equality below compares two runs of the same
    // emitter: if the URigVMInvokeEntryNode arm in CRIRDecompiler.cpp stopped
    // reading the entry name, both texts would carry the same nameless
    // `invoke_entry` and the comparison would still pass while the target lost
    // the SubRoutine target. Same for the exec wire, which the emitter is the
    // only thing writing.
    TestTrue(TEXT("decompile #1 emits the invoke_entry target name"),
        Result1.CRIRText.Contains(TEXT("= invoke_entry SubRoutine")));
    TestTrue(TEXT("decompile #1 emits the ExecuteContext wire argument"),
        Result1.CRIRText.Contains(TEXT("wire_in_ExecuteContext=%")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile invoke_entry (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("invoke_entry round-trip text equality"), Normalized2, Normalized1);
    return true;
}
