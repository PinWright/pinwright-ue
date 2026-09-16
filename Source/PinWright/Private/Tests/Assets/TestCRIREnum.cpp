// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR enum round-trip regression. An enum node referencing /Script/CoreUObject
// .EAxis (a stable core enum) is added, its EnumValue pin default is set to
// "X", and the source/target CRIR texts must round-trip byte-equal.
//
// Counterfactual: if the decompiler's URigVMEnumNode branch is removed so the
// node falls through to the TODO comment, the recompile loses the enum entirely
// — Result2 has no enum line and the equality assertion fails.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIREnum_RoundTrip,
    "PinWright.CRIR.RoundTrip.Enum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIREnum_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIREnum_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIREnum_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIREnum_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIREnum_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR enum: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIREnum_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR enum: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* EnumNode = SourceController->AddEnumNode(
        FName(TEXT("/Script/CoreUObject.EAxis")),
        FVector2D(0, 0),
        FString(TEXT("AxisEnum")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!EnumNode)
    {
        // Fallback path: AddEnumNode could not resolve EAxis on this engine
        // configuration. Skip rather than fail — the rest of the suite still
        // covers the parser/emitter/compiler glue.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("node-construction-failed"),
            TEXT("CRIR enum: AddEnumNode returned null for /Script/CoreUObject.EAxis — skipped."));
        return true;
    }
    const FString PinPath = FString::Printf(TEXT("%s.EnumValue"), *EnumNode->GetName());
    SourceController->SetPinDefaultValue(PinPath, TEXT("X"),
        /*bResizeArrays*/ true,
        /*bSetupUndoRedo*/ false,
        /*bMergeUndoAction*/ false,
        /*bPrintPythonCommand*/ false);

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Independent reference. Byte-equality below compares two runs of the same
    // emitter, so anything the emitter stops writing is missing from both texts
    // and the comparison still passes: dropping either read in
    // CRIRDecompiler.cpp's URigVMEnumNode arm — `EnumNode->GetCPPTypeObject()`
    // or `EnumNode->GetDefaultValue()` — loses the enum's type or its authored
    // value with this test green. These expectations come from the fixture above.
    TestTrue(TEXT("decompile #1 emits the enum's type path"),
        Result1.CRIRText.Contains(TEXT("= enum /Script/CoreUObject.EAxis")));
    TestTrue(TEXT("decompile #1 carries the authored enum value X"),
        Result1.CRIRText.Contains(TEXT("/Script/CoreUObject.EAxis = X")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile enum (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("enum round-trip text equality"), Normalized2, Normalized1);
    return true;
}
