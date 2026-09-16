// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR reroute round-trip regression. Builds a synthetic Control Rig BP with
// BeginExecution → reroute(float) wired (acting as a pass-through on the Weight
// pin), decompiles to CRIR text, compiles into a fresh empty target BP,
// decompiles again, and asserts byte-equal CRIR text.
//
// Counterfactual: if the ECRIROpcode::Reroute case is removed from
// CompileRigGraphBlock's Pass A switch, the compile-into-target step falls
// through to the default arm and returns CRIR_UNSUPPORTED_OPCODE, breaking
// the bSuccess assertion below.
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
#include "Units/RigUnit.h"
#include "Utils/AssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRReroute_RoundTrip,
    "PinWright.CRIR.RoundTrip.Reroute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRReroute_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRReroute_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRReroute_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRReroute_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRReroute_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        /*TargetSkeleton*/ nullptr,
        CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR reroute round-trip: source BP unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRReroute_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        /*TargetSkeleton*/ nullptr,
        CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR reroute round-trip: target BP unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source model controller available"), SourceController);
    if (!SourceController) { return false; }

    // BeginExecution → reroute(float) literal. Single-node reroute with
    // literal value is the dominant authored shape; wiring is exercised by
    // the broader RoundTripForwardsSolve test.
    URigVMNode* BeginNode = SourceController->AddUnitNodeFromStructPath(
        TEXT("/Script/ControlRig.RigUnit_BeginExecution"),
        FRigUnit::GetMethodName(),
        FVector2D(-200, 0),
        FString(TEXT("BeginExecution")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("BeginExecution created"), BeginNode);

    URigVMNode* RerouteNode = SourceController->AddFreeRerouteNode(
        TEXT("float"),
        /*InCPPTypeObjectPath*/ NAME_None,
        /*bIsConstant*/ false,
        /*InCustomWidgetName*/ NAME_None,
        /*InDefaultValue*/ TEXT("1.500000"),
        FVector2D(120, 40),
        /*InNodeName*/ FString(TEXT("RerouteFloat")),
        /*bSetupUndoRedo*/ false);
    TestNotNull(TEXT("free reroute created"), RerouteNode);
    if (!BeginNode || !RerouteNode) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 succeeds (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Independent reference. The byte-equality check at the bottom of this test
    // cannot see the reroute literal being dropped: both decompiles run the same
    // emitter, so a value the emitter stops reading is missing from BOTH texts and
    // they stay byte-equal. The gate that decides whether the literal is emitted is
    //
    //     CRIRDecompiler.cpp, URigVMRerouteNode arm:
    //         if (PinHasAuthoredDefaultOverride(Pin)) { DefaultValue = Pin->GetDefaultValue(); }
    //
    // That predicate is deliberately NOT `URigVMPin::HasDefaultValueOverride()`,
    // which short-circuits to false whenever the `RigVM.EnablePinOverrides` console
    // variable is off — its shipped default (RigVMPin.cpp:34
    // `TAutoConsoleVariable<bool> CVarRigVMEnablePinOverrides(
    // TEXT("RigVM.EnablePinOverrides"), false, ...)`), and nothing in this project or
    // the engine's ini files turns it on. While the engine predicate was used here,
    // this assertion was red and CRIR dropped every authored pin literal on every
    // node kind. If it goes red again, the reroute literal authored two statements
    // above is being silently dropped from the CRIR wire format — a product defect,
    // not a bad expectation. Fix the decompiler, not this test.
    // Asserted as two substrings rather than one: the CPPType token the emitter
    // writes between `reroute` and the literal is the engine's spelling of the
    // pin type, which is not the property under test here.
    TestTrue(TEXT("decompile #1 emits a reroute instruction"),
        Result1.CRIRText.Contains(TEXT(" = reroute ")));
    // The failure text names the defect, not just the symptom, because the suite
    // output is the only place a reader sees it. Without that, the obvious "fix" is
    // to delete the assertion — which restores a test that cannot fail.
    TestTrue(TEXT("decompile #1 carries the authored reroute literal 1.500000 "
                  "(if this is RED: CRIRDecompiler.cpp's pin-literal gate has been "
                  "reverted to URigVMPin::HasDefaultValueOverride(), which returns false "
                  "whenever the RigVM.EnablePinOverrides CVar is off, and false is its "
                  "shipped default. CRIR is dropping EVERY authored pin literal. "
                  "Fix the decompiler, not this test.)"),
        Result1.CRIRText.Contains(TEXT("= 1.500000")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile reroute to target succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 succeeds (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("reroute round-trip text equality"), Normalized2, Normalized1);
    return true;
}
