// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR function-library round-trip regression. Builds a function in the local
// library, references it from the main graph, then round-trips via CRIR.
//
// Counterfactual: If the decompiler still filters with
// bIncludeFunctionLibrary=false (and doesn't separately emit the library via
// GetLocalFunctionLibrary), Result1 omits the `rig_function` block and
// Result2's function_ref resolves to null because there is nothing to reference.
//
// The byte-equality assertion at the end is blind to anything both decompiles
// drop together. In particular, if the `rig_function` body loop in
// CRIRDecompiler.cpp (the block that follows EmitRigFunctionHeader) stopped
// emitting the function's contained instructions, decompile #1 would carry an
// empty `rig_function "MyFunc" { }`, the compiler would build an empty function
// in the target, and decompile #2 would match it byte-for-byte — the inner
// RigVMFunction_MathIntAdd node would vanish with the test still green. The
// content assertions below are written from what this test *authored*, and the
// target-side assertions read the target Blueprint rather than its re-emission.
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
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Units/RigUnit.h"
#include "Utils/AssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRFunctionRef_LibraryAndRef_RoundTrip,
    "PinWright.CRIR.RoundTrip.FunctionRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRFunctionRef_LibraryAndRef_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRFuncRef_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRFuncRef_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRFuncRef_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRFuncRef_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR function ref: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRFuncRef_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR function ref: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    FRigVMClient* Client = GetControlRigRigVMClient(SourceBP);
    TestNotNull(TEXT("source client"), Client);
    if (!Client) { return false; }

    // On UE 5.3 a freshly created Control Rig Blueprint has no local function library yet
    // (GetFunctionLibrary() returns null until first use); 5.4+ pre-creates it. GetOrCreateFunctionLibrary
    // returns the existing library on 5.4+ and lazily creates it on 5.3, keeping this setup version-agnostic.
    URigVMFunctionLibrary* Library = Client->GetOrCreateFunctionLibrary(/*bSetupUndoRedo*/ false);
    TestNotNull(TEXT("source local function library"), Library);
    if (!Library) { return false; }

    // GetOrCreateController materializes the library's controller on 5.3 (where the lazily
    // created library has no registered controller yet); returns the existing one on 5.4+.
    URigVMController* LibController = Client->GetOrCreateController(Library);
    TestNotNull(TEXT("source library controller"), LibController);
    if (!LibController) { return false; }

    URigVMLibraryNode* FuncNode = LibController->AddFunctionToLibrary(
        FName(TEXT("MyFunc")),
        /*bMutable*/ true,
        FVector2D::ZeroVector,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("function node added"), FuncNode);
    if (!FuncNode) { return false; }

    URigVMController* InnerController = Client->GetOrCreateController(FuncNode->GetContainedGraph());
    TestNotNull(TEXT("inner controller"), InnerController);
    if (!InnerController) { return false; }

    // Populate the function body with a unit node so it's non-trivial. Must be
    // a non-event unit — event nodes (e.g. RigUnit_BeginExecution) are rejected
    // by the controller when added to anything other than a top-level graph
    // ("Event nodes can only be added to top level graphs.").
    URigVMNode* InnerUnit = InnerController->AddUnitNodeFromStructPath(
        TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"),
        FRigUnit::GetMethodName(),
        FVector2D(0, 0),
        FString(TEXT("InnerAdd")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("inner unit"), InnerUnit);
    if (!InnerUnit) { return false; }

    URigVMGraph* MainModel = nullptr;
    URigVMController* MainController = GetFirstModelController(SourceBP, MainModel);
    TestNotNull(TEXT("main controller"), MainController);
    if (!MainController) { return false; }

    URigVMNode* RefNode = MainController->AddFunctionReferenceNode(
        FuncNode,
        FVector2D(100, 100),
        FString(TEXT("MyFuncRef")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("function reference"), RefNode);
    if (!RefNode) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Independent reference: expectations written from the fixture above, not
    // from the emitter's output.
    TestTrue(TEXT("decompile #1 emits the rig_function block for MyFunc"),
        Result1.CRIRText.Contains(TEXT("rig_function \"MyFunc\"")));
    TestTrue(TEXT("decompile #1 emits the function body's inner unit node"),
        Result1.CRIRText.Contains(TEXT("/Script/RigVM.RigVMFunction_MathIntAdd")));
    // Same-asset references must emit the bare `function_ref <Name>` form; the
    // `<HostPath>::<Name>` form is reserved for external references. Both forms
    // round-trip byte-equal, so only this direct check discriminates them.
    TestTrue(TEXT("decompile #1 emits the same-asset function_ref form"),
        Result1.CRIRText.Contains(TEXT("= function_ref MyFunc")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile function ref (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    // Independent reference: the target Blueprint itself. Byte-equal text only
    // proves the two decompiles agree; these read the compiled asset.
    FRigVMClient* TgtClient = GetControlRigRigVMClient(TargetBP);
    URigVMFunctionLibrary* TgtLibrary = TgtClient ? TgtClient->GetFunctionLibrary() : nullptr;
    TestNotNull(TEXT("target function library"), TgtLibrary);
    if (TgtLibrary)
    {
        URigVMLibraryNode* TgtFunc = TgtLibrary->FindFunction(FName(TEXT("MyFunc")));
        TestNotNull(TEXT("target library contains MyFunc"), TgtFunc);
        if (TgtFunc)
        {
            URigVMGraph* TgtInner = TgtFunc->GetContainedGraph();
            TestNotNull(TEXT("target MyFunc has a contained graph"), TgtInner);
            if (TgtInner)
            {
                // The authored body is one RigVMFunction_MathIntAdd unit node.
                // Asserting its presence (not merely a non-empty graph) is what
                // catches a function body that round-tripped as empty.
                bool bFoundInnerAdd = false;
                for (URigVMNode* Node : TgtInner->GetNodes())
                {
                    const URigVMUnitNode* AsUnit = Cast<URigVMUnitNode>(Node);
                    const UScriptStruct* Struct = AsUnit ? AsUnit->GetScriptStruct() : nullptr;
                    if (Struct && Struct->GetPathName() == TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"))
                    {
                        bFoundInnerAdd = true;
                        break;
                    }
                }
                TestTrue(TEXT("target MyFunc body carries the authored RigVMFunction_MathIntAdd node"),
                    bFoundInnerAdd);
            }
        }
    }

    URigVMGraph* TargetMainModel = nullptr;
    GetFirstModelController(TargetBP, TargetMainModel);
    TestNotNull(TEXT("target main model"), TargetMainModel);
    if (TargetMainModel)
    {
        bool bFoundRef = false;
        for (URigVMNode* Node : TargetMainModel->GetNodes())
        {
            if (Cast<URigVMFunctionReferenceNode>(Node) != nullptr)
            {
                bFoundRef = true;
                break;
            }
        }
        TestTrue(TEXT("target main graph carries a function reference node"), bFoundRef);
    }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("function ref round-trip text equality"), Normalized2, Normalized1);
    return true;
}
