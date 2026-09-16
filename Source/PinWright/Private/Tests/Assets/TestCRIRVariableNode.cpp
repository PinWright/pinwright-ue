// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR variable node round-trip regression. A get-variable node referencing a
// member variable of type bool is added on the source rig; both source and
// target BPs pre-register the same member variable via URigVMBlueprint::
// AddMemberVariable so URigVMController::AddVariableNode can resolve it via
// the GetExternalVariablesDelegate during the compile pass — that path reads
// the generated-class CDO, which only exists after the synchronous compile
// AddMemberVariable triggers internally.
//
// Counterfactual: if the URigVMVariableNode arm in CRIRDecompiler.cpp (around
// line 692) is removed, the `var` line is dropped from Result1.CRIRText and
// the round-tripped target has no variable node — the byte-equal assertion
// fails.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRVariable_RoundTrip,
    "PinWright.CRIR.RoundTrip.Variable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRVariable_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRVar_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRVar_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRVar_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRVar_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR variable: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRVar_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR variable: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    // URigVMController::AddVariableNode resolves via GetExternalVariablesDelegate,
    // which reads the generated-class CDO. FBlueprintEditorUtils::AddMemberVariable
    // populates NewVariables but never regenerates the class, so the delegate sees
    // nothing and the controller logs "variable does not exist". URigVMBlueprint::
    // AddMemberVariable goes through the canonical RigVM path: registers the K2
    // member and runs FBlueprintCompilationManager::CompileSynchronously so the
    // CDO exposes the property.
    const FName VarName(TEXT("MyBool"));
    const FName SourceAdded = SourceBP->AddMemberVariable(VarName, TEXT("bool"));
    const FName TargetAdded = TargetBP->AddMemberVariable(VarName, TEXT("bool"));
    TestEqual(TEXT("source member variable registered"), SourceAdded, VarName);
    TestEqual(TEXT("target member variable registered"), TargetAdded, VarName);

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* VarNode = SourceController->AddVariableNode(
        VarName, TEXT("bool"), /*InCPPTypeObject*/ nullptr, /*bIsGetter*/ true,
        /*InDefaultValue*/ FString(), FVector2D(0, 0),
        /*InNodeName*/ FString(TEXT("GetMyBool")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!VarNode)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("node-construction-failed"),
            TEXT("CRIR variable: AddVariableNode returned null on source — skipped."));
        return true;
    }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Anchor the counterfactual: if the URigVMVariableNode arm in CRIRDecompiler.cpp
    // (~line 692) is removed, both Result1 and Result2 silently lose the var line and
    // byte-equality would still hold — so equality alone doesn't catch arm regression.
    // Asserting the var line is present in Result1 makes the test fail loudly on
    // decompile-arm removal.
    TestTrue(TEXT("var line emitted for MyBool in Result1"),
        Result1.CRIRText.Contains(TEXT("var MyBool bool")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile variable (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("variable round-trip text equality"), Normalized2, Normalized1);
    return true;
}
