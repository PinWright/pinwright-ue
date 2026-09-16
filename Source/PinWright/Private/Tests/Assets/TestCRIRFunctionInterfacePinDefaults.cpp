// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR function-interface pin-default reconciliation regression. Verifies that
// non-default values on function_entry pins survive a CRIR round-trip — the
// compiler must locate the auto-created entry node and replay pin defaults onto
// it rather than trying to Add* a duplicate.
//
// Counterfactual: If the compiler tries to Add* an entry node instead of
// finding the auto-created one, compile fails with duplicate/conflict; if it
// skips pin replay entirely, the post-compile read returns the wildcard default,
// not 42.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRFunctionInterface_PinDefaults_Reconcile,
    "PinWright.CRIR.RoundTrip.FunctionInterfacePinDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRFunctionInterface_PinDefaults_Reconcile::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRFnPin_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRFnPin_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRFnPin_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRFnPin_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR function pin defaults: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRFnPin_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR function pin defaults: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    FRigVMClient* Client = GetControlRigRigVMClient(SourceBP);
    // On UE 5.3 a freshly created Control Rig Blueprint has no local function library yet
    // (GetFunctionLibrary() returns null until first use); 5.4+ pre-creates it. GetOrCreateFunctionLibrary
    // returns the existing library on 5.4+ and lazily creates it on 5.3, keeping this setup version-agnostic.
    URigVMFunctionLibrary* Library = Client ? Client->GetOrCreateFunctionLibrary(/*bSetupUndoRedo*/ false) : nullptr;
    // GetOrCreateController materializes the library's controller on 5.3 (where the lazily
    // created library has no registered controller yet); returns the existing one on 5.4+.
    URigVMController* LibController = (Client && Library) ? Client->GetOrCreateController(Library) : nullptr;
    TestNotNull(TEXT("library controller"), LibController);
    if (!LibController) { return false; }

    URigVMLibraryNode* FuncNode = LibController->AddFunctionToLibrary(
        FName(TEXT("FnWithThreshold")),
        /*bMutable*/ true,
        FVector2D::ZeroVector,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("function node added"), FuncNode);
    if (!FuncNode) { return false; }

    URigVMGraph* InnerGraph = FuncNode->GetContainedGraph();
    URigVMController* InnerController = Client->GetOrCreateController(InnerGraph);
    TestNotNull(TEXT("inner controller"), InnerController);
    if (!InnerController) { return false; }

    // Add an int32 input pin to the function (exposed on entry node).
    InnerController->AddExposedPin(
        FName(TEXT("Threshold")),
        ERigVMPinDirection::Input,
        TEXT("int32"),
        NAME_None,
        TEXT("42"),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);

    // Sanity: pin default on the source entry is 42.
    URigVMFunctionEntryNode* SrcEntry = InnerGraph->GetEntryNode();
    TestNotNull(TEXT("source entry"), SrcEntry);
    if (!SrcEntry) { return false; }
    URigVMPin* SrcThresholdPin = SrcEntry->FindPin(TEXT("Threshold"));
    TestNotNull(TEXT("source Threshold pin"), SrcThresholdPin);
    if (!SrcThresholdPin) { return false; }
    TestEqual(TEXT("source Threshold default is 42"), SrcThresholdPin->GetDefaultValue(), FString(TEXT("42")));

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Confirm CRIR text contains the expected function_entry pin default.
    TestTrue(TEXT("CRIR text mentions Threshold=42"),
        Result1.CRIRText.Contains(TEXT("Threshold=42")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FRigVMClient* TgtClient = GetControlRigRigVMClient(TargetBP);
    URigVMFunctionLibrary* TgtLibrary = TgtClient ? TgtClient->GetFunctionLibrary() : nullptr;
    TestNotNull(TEXT("target library"), TgtLibrary);
    if (!TgtLibrary) { return false; }
    URigVMLibraryNode* TgtFunc = TgtLibrary->FindFunction(FName(TEXT("FnWithThreshold")));
    TestNotNull(TEXT("target function node"), TgtFunc);
    if (!TgtFunc) { return false; }
    URigVMGraph* TgtInner = TgtFunc->GetContainedGraph();
    URigVMFunctionEntryNode* TgtEntry = TgtInner ? TgtInner->GetEntryNode() : nullptr;
    TestNotNull(TEXT("target entry"), TgtEntry);
    if (!TgtEntry) { return false; }
    URigVMPin* TgtThresholdPin = TgtEntry->FindPin(TEXT("Threshold"));
    TestNotNull(TEXT("target Threshold pin"), TgtThresholdPin);
    if (!TgtThresholdPin) { return false; }
    TestEqual(TEXT("target Threshold default reconciled to 42"),
        TgtThresholdPin->GetDefaultValue(), FString(TEXT("42")));
    return true;
}
