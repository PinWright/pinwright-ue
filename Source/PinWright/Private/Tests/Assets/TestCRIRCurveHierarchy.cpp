// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for Control Rig hierarchy curves: curves are flat named float
// elements, not keyed animation curves.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCurveHierarchyRoundTripTest,
    "PinWright.CRIR.RoundTrip.CurveHierarchy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRCurveHierarchyRoundTripTest::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRCurve_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRCurve_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRCurve_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRCurve_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr,
        CreateError));
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR curve round-trip: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRCurve_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr,
        CreateError));
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR curve round-trip: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigHierarchyController* SourceHierarchyController = SourceBP->GetHierarchyController();
    TestNotNull(TEXT("source hierarchy controller"), SourceHierarchyController);
    if (!SourceHierarchyController) { return false; }

    URigHierarchy* SourceHierarchy = GetControlRigHierarchy(SourceBP);
    TestNotNull(TEXT("source hierarchy"), SourceHierarchy);
    if (!SourceHierarchy) { return false; }

    const FRigElementKey CurveKey = SourceHierarchyController->AddCurve(
        FName(TEXT("IKBlend")),
        0.75f,
        /*bSetupUndo*/ false,
        /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("source curve added"), CurveKey.Type == ERigElementType::Curve);
    if (CurveKey.Type != ERigElementType::Curve) { return false; }

    SourceHierarchy->SetCurveValue(CurveKey, 0.75f, /*bSetupUndo*/ false);

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    TestTrue(TEXT("CRIR contains flat curve value"),
        Result1.CRIRText.Contains(TEXT("curve IKBlend value=0.75")));
    TestFalse(TEXT("CRIR does not emit curve TODO"),
        Result1.CRIRText.Contains(TEXT("unsupported element kind: Curve")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile curve hierarchy (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("curve hierarchy round-trip text equality"), Normalized2, Normalized1);
    return true;
}
