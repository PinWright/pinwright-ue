// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR if/select round-trip regression. One If node and one Select node are
// added via the controller (both are URigVMDispatchNode instances today —
// AddIfNode / AddSelectNode dispatch through FRigVMDispatch_If /
// FRigVMDispatch_SelectInt32 templates), decompiled, recompiled into a fresh
// target, decompiled again, and the two texts must be byte-equal.
//
// Counterfactual: if the decompiler's URigVMDispatchNode discriminator (factory
// script-struct name check for "RigVMDispatch_If" / "RigVMDispatch_Select*") is
// replaced by a fallback to the TODO emit, both nodes emit as
// `# TODO unsupported node kind: RigVMDispatchNode` on the source side. The
// compile-back loses them and Result2 has no if/select lines — breaking byte
// equality with Result1.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRIfSelect_RoundTrip,
    "PinWright.CRIR.RoundTrip.IfSelect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRIfSelect_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRIfSel_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRIfSel_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRIfSel_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRIfSel_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR if/select: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRIfSel_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR if/select: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* IfNode = SourceController->AddIfNode(
        TEXT("float"), NAME_None,
        FVector2D(0, 0),
        FString(TEXT("IfFloat")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("if created"), IfNode);

    URigVMNode* SelectNode = SourceController->AddSelectNode(
        TEXT("int32"), NAME_None,
        FVector2D(0, 200),
        FString(TEXT("SelectInt")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("select created"), SelectNode);
    if (!IfNode || !SelectNode) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Independent reference. If/Select are URigVMDispatchNode instances that
    // CRIRDecompiler.cpp discriminates by factory struct name into the dedicated
    // `if` / `select` opcodes. Losing that discrimination — letting both fall
    // through to the generic `dispatch <Factory>` arm — is invisible to the
    // byte-equality check below, because both decompiles would then emit the
    // generic form and the compiler would rebuild dispatch nodes from it. The
    // opcode is the whole point of this test, so it is asserted directly.
    TestTrue(TEXT("decompile #1 emits the dedicated `if` opcode"),
        Result1.CRIRText.Contains(TEXT("= if ")));
    TestTrue(TEXT("decompile #1 emits the dedicated `select` opcode"),
        Result1.CRIRText.Contains(TEXT("= select ")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile if/select (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("if/select round-trip text equality"), Normalized2, Normalized1);
    return true;
}
