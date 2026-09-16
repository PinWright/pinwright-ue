// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for B-crir-compile-failure-not-atomic. A failure in the graph
// pass must roll back mutations already made to the hierarchy and function
// library, as well as the replace-mode clear of the target graph.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "EdGraph/RigVMEdGraph.h"
#include "Editor.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Units/RigUnit.h"
#include "Utils/AssetUtils.h"
#include "Utils/ControlRigBlueprintCompat.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCompileFailureRollbackAtomicityTest,
    "PinWright.CRIR.Compile.FailureRollbackAtomicity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRCompileFailureRollbackAtomicityTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEditor->Trans)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-transactor"),
            TEXT("CRIR atomic rollback requires an editor transactor - skipped."));
        return true;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Name = FString::Printf(TEXT("CR_CRIRAtomic_%s"), *Guid);
    const FString AssetPath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *Name);
    const FString AssetObject = FString::Printf(TEXT("%s.%s"), *AssetPath, *Name);

    ON_SCOPE_EXIT
    {
        GEditor->ResetTransaction(
            NSLOCTEXT("CRIRAtomicityTest", "CleanupReset", "CRIR Atomicity Test Cleanup"));
        CleanupTestAsset(AssetPath);
    };

    FString CreateError;
    UControlRigBlueprint* BP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        Name, TEXT("/Game/PinWrightTests"), /*TargetSkeleton*/ nullptr, CreateError));
    if (!BP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR atomic rollback: could not create Control Rig BP (%s) - skipped."),
                *CreateError));
        return true;
    }

    URigVMGraph* Model = nullptr;
    URigVMController* ModelController = GetFirstModelController(BP, Model);
    if (!TestNotNull(TEXT("model controller"), ModelController)
        || !TestNotNull(TEXT("model"), Model))
    {
        return false;
    }

    URigVMNode* BaselineComment = ModelController->AddCommentNode(
        TEXT("Rollback baseline"),
        FVector2D(25.0, 50.0),
        FVector2D(300.0, 180.0),
        FLinearColor(0.1f, 0.2f, 0.3f, 0.8f),
        TEXT("RollbackBaselineComment"),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!TestNotNull(TEXT("baseline graph comment"), BaselineComment))
    {
        return false;
    }

    URigVMNode* FirstAdd = ModelController->AddUnitNodeFromStructPath(
        TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"),
        FRigUnit::GetMethodName(),
        FVector2D(-200.0, 0.0),
        TEXT("RollbackFirstAdd"),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    URigVMNode* SecondAdd = ModelController->AddUnitNodeFromStructPath(
        TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"),
        FRigUnit::GetMethodName(),
        FVector2D(200.0, 0.0),
        TEXT("RollbackSecondAdd"),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!TestNotNull(TEXT("first linked baseline node"), FirstAdd)
        || !TestNotNull(TEXT("second linked baseline node"), SecondAdd))
    {
        return false;
    }

    const FString FirstResultPath = FString::Printf(TEXT("%s.Result"), *FirstAdd->GetName());
    const FString SecondInputPath = FString::Printf(TEXT("%s.A"), *SecondAdd->GetName());
    if (!TestTrue(TEXT("baseline nodes linked"), ModelController->AddLink(
            FirstResultPath,
            SecondInputPath,
            /*bSetupUndoRedo*/ false,
            /*bPrintPythonCommand*/ false)))
    {
        return false;
    }

    const FString SecondDefaultPath = FString::Printf(TEXT("%s.B"), *SecondAdd->GetName());
    if (!TestTrue(TEXT("baseline non-default pin value authored"), ModelController->SetPinDefaultValue(
            SecondDefaultPath,
            TEXT("41"),
            /*bResizeArrays*/ true,
            /*bSetupUndoRedo*/ false,
            /*bMergeUndoAction*/ false,
            /*bPrintPythonCommand*/ false)))
    {
        return false;
    }
    URigVMPin* BaselineDefaultPin = SecondAdd->FindPin(TEXT("B"));
    if (!TestNotNull(TEXT("baseline default pin"), BaselineDefaultPin))
    {
        return false;
    }
    const FString BaselineDefaultValue = BaselineDefaultPin->GetDefaultValue();

    const FString BaselineCollapseNodeName(TEXT("RollbackBaselineCollapse"));
    const FString BaselineNestedFirstNodeName(TEXT("RollbackNestedFirst"));
    const FString BaselineNestedSecondNodeName(TEXT("RollbackNestedSecond"));
    const FName BaselineCollapseName(*BaselineCollapseNodeName);
    const FName BaselineNestedFirstName(*BaselineNestedFirstNodeName);
    const FName BaselineNestedSecondName(*BaselineNestedSecondNodeName);
    URigVMNode* BaselineNestedFirst = ModelController->AddUnitNodeFromStructPath(
        TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"),
        FRigUnit::GetMethodName(),
        FVector2D(-200.0, 300.0),
        BaselineNestedFirstNodeName,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    URigVMNode* BaselineNestedSecond = ModelController->AddUnitNodeFromStructPath(
        TEXT("/Script/RigVM.RigVMFunction_MathIntAdd"),
        FRigUnit::GetMethodName(),
        FVector2D(200.0, 300.0),
        BaselineNestedSecondNodeName,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!TestNotNull(TEXT("baseline nested first node"), BaselineNestedFirst)
        || !TestNotNull(TEXT("baseline nested second node"), BaselineNestedSecond))
    {
        return false;
    }

    if (!TestTrue(TEXT("baseline nested nodes linked"), ModelController->AddLink(
            FString::Printf(TEXT("%s.Result"), *BaselineNestedFirst->GetName()),
            FString::Printf(TEXT("%s.A"), *BaselineNestedSecond->GetName()),
            /*bSetupUndoRedo*/ false,
            /*bPrintPythonCommand*/ false)))
    {
        return false;
    }

    const FString BaselineNestedDefaultPath = FString::Printf(
        TEXT("%s.B"), *BaselineNestedSecond->GetName());
    if (!TestTrue(TEXT("baseline nested non-default pin value authored"),
            ModelController->SetPinDefaultValue(
                BaselineNestedDefaultPath,
                TEXT("17"),
                /*bResizeArrays*/ true,
                /*bSetupUndoRedo*/ false,
                /*bMergeUndoAction*/ false,
                /*bPrintPythonCommand*/ false)))
    {
        return false;
    }

    TArray<FName> BaselineNestedNodeNames = {
        BaselineNestedFirstName,
        BaselineNestedSecondName};
    URigVMCollapseNode* BaselineCollapse = ModelController->CollapseNodes(
        BaselineNestedNodeNames,
        BaselineCollapseNodeName,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false,
        /*bIsAggregate*/ false);
    if (!TestNotNull(TEXT("baseline nested collapse node"), BaselineCollapse))
    {
        return false;
    }

    URigVMGraph* BaselineNestedGraph = BaselineCollapse->GetContainedGraph();
    if (!TestNotNull(TEXT("baseline nested contained graph"), BaselineNestedGraph))
    {
        return false;
    }
    URigVMNode* BaselineNestedFirstRestored = BaselineNestedGraph->FindNodeByName(
        BaselineNestedFirstName);
    URigVMNode* BaselineNestedSecondRestored = BaselineNestedGraph->FindNodeByName(
        BaselineNestedSecondName);
    URigVMPin* BaselineNestedResultPin = BaselineNestedFirstRestored
        ? BaselineNestedFirstRestored->FindPin(TEXT("Result"))
        : nullptr;
    URigVMPin* BaselineNestedInputPin = BaselineNestedSecondRestored
        ? BaselineNestedSecondRestored->FindPin(TEXT("A"))
        : nullptr;
    URigVMPin* BaselineNestedDefaultPin = BaselineNestedSecondRestored
        ? BaselineNestedSecondRestored->FindPin(TEXT("B"))
        : nullptr;
    if (!TestNotNull(TEXT("baseline nested first node moved into graph"), BaselineNestedFirstRestored)
        || !TestNotNull(TEXT("baseline nested second node moved into graph"), BaselineNestedSecondRestored)
        || !TestNotNull(TEXT("baseline nested source pin"), BaselineNestedResultPin)
        || !TestNotNull(TEXT("baseline nested target pin"), BaselineNestedInputPin)
        || !TestNotNull(TEXT("baseline nested default pin"), BaselineNestedDefaultPin))
    {
        return false;
    }
    const int32 BaselineNestedGraphNodeCount = BaselineNestedGraph->GetNodes().Num();
    const FString BaselineNestedGraphName = BaselineNestedGraph->GetGraphName();
    const FString BaselineNestedDefaultValue = BaselineNestedDefaultPin->GetDefaultValue();
    TestTrue(TEXT("baseline nested link authored"),
        BaselineNestedResultPin->IsLinkedTo(BaselineNestedInputPin));

    URigHierarchyController* HierarchyController = BP->GetHierarchyController();
    URigHierarchy* Hierarchy = GetControlRigHierarchy(BP);
    if (!TestNotNull(TEXT("hierarchy controller"), HierarchyController)
        || !TestNotNull(TEXT("hierarchy"), Hierarchy))
    {
        return false;
    }
    const FRigElementKey BaselineBone = HierarchyController->AddBone(
        FName(TEXT("BaselineRoot")),
        FRigElementKey(),
        FTransform::Identity,
        /*bTransformInGlobal*/ false,
        ERigBoneType::User,
        /*bSetupUndo*/ false,
        /*bPrintPythonCommand*/ false);
    if (!TestTrue(TEXT("baseline hierarchy bone added"), BaselineBone.Type == ERigElementType::Bone))
    {
        return false;
    }

    FRigVMClient* Client = GetControlRigRigVMClient(BP);
    URigVMFunctionLibrary* FunctionLibrary = Client
        ? Client->GetOrCreateFunctionLibrary(/*bSetupUndoRedo*/ false)
        : nullptr;
    if (!TestNotNull(TEXT("function library"), FunctionLibrary))
    {
        return false;
    }

    const int32 BaselineGraphNodeCount = Model->GetNodes().Num();
    const int32 BaselineFunctionCount = FunctionLibrary->GetNodes().Num();
    const TArray<FRigElementKey> BaselineHierarchyKeys = Hierarchy->GetAllKeys(/*bTraverse*/ false);
    const FCRIRDecompileResult BaselineDecompile = FCRIRDecompiler(BP).Decompile();
    if (!TestTrue(FString::Printf(TEXT("baseline decompile succeeds (code='%s')"),
            *BaselineDecompile.ErrorCode), BaselineDecompile.bSuccess))
    {
        return false;
    }

    UPackage* const Package = BP->GetOutermost();
    if (!TestNotNull(TEXT("target package"), Package))
    {
        return false;
    }
    auto AssertNoOrphanNodes = [this](const FString& ModeLabel, URigVMGraph* Graph)
    {
        TArray<UObject*> DirectGraphObjects;
        GetObjectsWithOuter(Graph, DirectGraphObjects, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS);

        int32 DirectNodeCount = 0;
        for (UObject* DirectObject : DirectGraphObjects)
        {
            if (URigVMNode* DirectNode = Cast<URigVMNode>(DirectObject))
            {
                ++DirectNodeCount;
                TestTrue(FString::Printf(TEXT("%s: direct node '%s' belongs to graph membership"),
                        *ModeLabel, *DirectNode->GetName()),
                    Graph->GetNodes().Contains(DirectNode));
            }
        }
        TestEqual(FString::Printf(TEXT("%s: no orphan nodes remain outered to graph"), *ModeLabel),
            DirectNodeCount, Graph->GetNodes().Num());
    };

    auto RunFailureCase = [&](ECRIRCompileMode Mode, const FString& ModeLabel) -> bool
    {
        const FString TransientBoneName = FString::Printf(TEXT("TransientBone%s"), *ModeLabel);
        const FString TransientFunctionName = FString::Printf(TEXT("TransientFunction%s"), *ModeLabel);
        const FString FailingCRIR = FString::Printf(
            TEXT("rig_hierarchy {\n")
            TEXT("    bone \"%s\"\n")
            TEXT("}\n")
            TEXT("rig_function \"%s\" {\n")
            TEXT("}\n")
            TEXT("rig_graph \"%s\" {\n")
            TEXT("    %%late = unit /Script/PinWright.DoesNotExist()\n")
            TEXT("}\n"),
            *TransientBoneName,
            *TransientFunctionName,
            *Model->GetGraphName());

        Package->SetDirtyFlag(false);

        FCRIRCompileOptions Options;
        Options.TargetAssetPath = AssetObject;
        Options.Mode = Mode;
        Options.bRunLayout = false;
        Options.bSave = false;

        const FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(FailingCRIR, Options);
        TestFalse(FString::Printf(TEXT("%s: late unit-creation failure is reported"), *ModeLabel),
            CompileResult.bSuccess);
        TestEqual(FString::Printf(TEXT("%s: late failure keeps its error code"), *ModeLabel),
            CompileResult.ErrorCode, FString(TEXT("CRIR_UNIT_CREATE_FAILED")));
        TestFalse(FString::Printf(TEXT("%s: failed compile restores clean package state"), *ModeLabel),
            Package->IsDirty());

        URigVMGraph* RestoredModel = nullptr;
        URigVMController* RestoredController = GetFirstModelController(BP, RestoredModel);
        if (!TestNotNull(FString::Printf(TEXT("%s: restored model controller"), *ModeLabel), RestoredController)
            || !TestNotNull(FString::Printf(TEXT("%s: restored model"), *ModeLabel), RestoredModel))
        {
            return false;
        }
        TestEqual(FString::Printf(TEXT("%s: graph node count restored"), *ModeLabel),
            RestoredModel->GetNodes().Num(), BaselineGraphNodeCount);
        TestFalse(FString::Printf(TEXT("%s: restored model has no stale null nodes"), *ModeLabel),
            RestoredModel->GetNodes().Contains(nullptr));

        URigVMCollapseNode* RestoredCollapse = Cast<URigVMCollapseNode>(
            RestoredModel->FindNodeByName(BaselineCollapseName));
        if (!TestNotNull(FString::Printf(TEXT("%s: baseline collapse restored"), *ModeLabel),
                RestoredCollapse))
        {
            return false;
        }
        URigVMGraph* RestoredNestedGraph = RestoredCollapse->GetContainedGraph();
        if (!TestNotNull(FString::Printf(TEXT("%s: baseline nested graph restored"), *ModeLabel),
                RestoredNestedGraph))
        {
            return false;
        }
        TestTrue(FString::Printf(TEXT("%s: restored nested graph is valid"), *ModeLabel),
            IsValid(RestoredNestedGraph));
        TestTrue(FString::Printf(TEXT("%s: restored nested graph has original parent"), *ModeLabel),
            RestoredNestedGraph->GetParentGraph() == RestoredModel);
        TestEqual(FString::Printf(TEXT("%s: restored nested graph name"), *ModeLabel),
            RestoredNestedGraph->GetGraphName(), BaselineNestedGraphName);
        TestEqual(FString::Printf(TEXT("%s: restored nested graph node count"), *ModeLabel),
            RestoredNestedGraph->GetNodes().Num(), BaselineNestedGraphNodeCount);
        TestFalse(FString::Printf(TEXT("%s: restored nested graph has no stale null nodes"), *ModeLabel),
            RestoredNestedGraph->GetNodes().Contains(nullptr));

        URigVMNode* RestoredNestedFirst = RestoredNestedGraph->FindNodeByName(
            BaselineNestedFirstName);
        URigVMNode* RestoredNestedSecond = RestoredNestedGraph->FindNodeByName(
            BaselineNestedSecondName);
        if (!TestNotNull(FString::Printf(TEXT("%s: nested first node restored"), *ModeLabel),
                RestoredNestedFirst)
            || !TestNotNull(FString::Printf(TEXT("%s: nested second node restored"), *ModeLabel),
                RestoredNestedSecond))
        {
            return false;
        }
        URigVMPin* RestoredNestedResultPin = RestoredNestedFirst->FindPin(TEXT("Result"));
        URigVMPin* RestoredNestedInputPin = RestoredNestedSecond->FindPin(TEXT("A"));
        URigVMPin* RestoredNestedDefaultPin = RestoredNestedSecond->FindPin(TEXT("B"));
        if (!TestNotNull(FString::Printf(TEXT("%s: nested source pin restored"), *ModeLabel),
                RestoredNestedResultPin)
            || !TestNotNull(FString::Printf(TEXT("%s: nested target pin restored"), *ModeLabel),
                RestoredNestedInputPin)
            || !TestNotNull(FString::Printf(TEXT("%s: nested default pin restored"), *ModeLabel),
                RestoredNestedDefaultPin))
        {
            return false;
        }
        TestTrue(FString::Printf(TEXT("%s: nested link restored"), *ModeLabel),
            RestoredNestedResultPin->IsLinkedTo(RestoredNestedInputPin));
        TestEqual(FString::Printf(TEXT("%s: nested non-default pin value restored"), *ModeLabel),
            RestoredNestedDefaultPin->GetDefaultValue(), BaselineNestedDefaultValue);
        AssertNoOrphanNodes(ModeLabel + TEXT(" nested graph"), RestoredNestedGraph);

        URigVMNode* RestoredComment = RestoredModel->FindNodeByName(FName(TEXT("RollbackBaselineComment")));
        TestNotNull(FString::Printf(TEXT("%s: baseline comment restored"), *ModeLabel), RestoredComment);
        if (RestoredComment)
        {
            TestTrue(FString::Printf(TEXT("%s: restored comment is valid"), *ModeLabel), IsValid(RestoredComment));
            TestTrue(FString::Printf(TEXT("%s: restored comment has original outer"), *ModeLabel),
                RestoredComment->GetOuter() == RestoredModel);
        }

        URigVMNode* RestoredFirstAdd = RestoredModel->FindNodeByName(FName(TEXT("RollbackFirstAdd")));
        URigVMNode* RestoredSecondAdd = RestoredModel->FindNodeByName(FName(TEXT("RollbackSecondAdd")));
        if (!TestNotNull(FString::Printf(TEXT("%s: first linked node restored"), *ModeLabel), RestoredFirstAdd)
            || !TestNotNull(FString::Printf(TEXT("%s: second linked node restored"), *ModeLabel), RestoredSecondAdd))
        {
            return false;
        }

        URigVMPin* RestoredResultPin = RestoredFirstAdd->FindPin(TEXT("Result"));
        URigVMPin* RestoredInputPin = RestoredSecondAdd->FindPin(TEXT("A"));
        URigVMPin* RestoredDefaultPin = RestoredSecondAdd->FindPin(TEXT("B"));
        if (!TestNotNull(FString::Printf(TEXT("%s: source link pin restored"), *ModeLabel), RestoredResultPin)
            || !TestNotNull(FString::Printf(TEXT("%s: target link pin restored"), *ModeLabel), RestoredInputPin)
            || !TestNotNull(FString::Printf(TEXT("%s: default pin restored"), *ModeLabel), RestoredDefaultPin))
        {
            return false;
        }
        TestTrue(FString::Printf(TEXT("%s: baseline link restored"), *ModeLabel),
            RestoredResultPin->IsLinkedTo(RestoredInputPin));
        TestEqual(FString::Printf(TEXT("%s: non-default pin value restored"), *ModeLabel),
            RestoredDefaultPin->GetDefaultValue(), BaselineDefaultValue);

        Hierarchy = GetControlRigHierarchy(BP);
        if (!TestNotNull(FString::Printf(TEXT("%s: restored hierarchy"), *ModeLabel), Hierarchy))
        {
            return false;
        }
        const TArray<FRigElementKey> RestoredHierarchyKeys = Hierarchy->GetAllKeys(/*bTraverse*/ false);
        TestEqual(FString::Printf(TEXT("%s: hierarchy element count restored"), *ModeLabel),
            RestoredHierarchyKeys.Num(), BaselineHierarchyKeys.Num());
        if (RestoredHierarchyKeys.Num() == BaselineHierarchyKeys.Num())
        {
            for (int32 Index = 0; Index < BaselineHierarchyKeys.Num(); ++Index)
            {
                TestTrue(FString::Printf(TEXT("%s: hierarchy key %d restored"), *ModeLabel, Index),
                    RestoredHierarchyKeys[Index] == BaselineHierarchyKeys[Index]);
            }
        }
        TestNull(FString::Printf(TEXT("%s: transient hierarchy element removed"), *ModeLabel),
            Hierarchy->Find(FRigElementKey(FName(*TransientBoneName), ERigElementType::Bone)));

        Client = GetControlRigRigVMClient(BP);
        FunctionLibrary = Client ? Client->GetFunctionLibrary() : nullptr;
        if (!TestNotNull(FString::Printf(TEXT("%s: restored function library"), *ModeLabel), FunctionLibrary))
        {
            return false;
        }
        TestEqual(FString::Printf(TEXT("%s: function library node count restored"), *ModeLabel),
            FunctionLibrary->GetNodes().Num(), BaselineFunctionCount);
        TestFalse(FString::Printf(TEXT("%s: restored function library has no stale null nodes"), *ModeLabel),
            FunctionLibrary->GetNodes().Contains(nullptr));
        TestNull(FString::Printf(TEXT("%s: transient function removed"), *ModeLabel),
            FunctionLibrary->FindNodeByName(FName(*TransientFunctionName)));

        AssertNoOrphanNodes(ModeLabel + TEXT(" model"), RestoredModel);
        AssertNoOrphanNodes(ModeLabel + TEXT(" function library"), FunctionLibrary);

        URigVMEdGraph* RestoredEdGraph = Cast<URigVMEdGraph>(BP->GetEdGraph(RestoredModel));
        if (!TestNotNull(FString::Printf(TEXT("%s: editor graph mirror exists"), *ModeLabel), RestoredEdGraph))
        {
            return false;
        }
        TestEqual(FString::Printf(TEXT("%s: editor/model node counts match"), *ModeLabel),
            RestoredEdGraph->Nodes.Num(), RestoredModel->GetNodes().Num());
        for (URigVMNode* ModelNode : RestoredModel->GetNodes())
        {
            TestNotNull(FString::Printf(TEXT("%s: editor mirror contains model node '%s'"),
                    *ModeLabel, *ModelNode->GetName()),
                RestoredEdGraph->FindNodeForModelNodeName(ModelNode->GetFName(), /*bCacheIfRequired*/ false));
        }

        const FCRIRDecompileResult RestoredDecompile = FCRIRDecompiler(BP).Decompile();
        if (!TestTrue(FString::Printf(TEXT("%s: post-rollback decompile succeeds (code='%s')"),
                *ModeLabel, *RestoredDecompile.ErrorCode), RestoredDecompile.bSuccess))
        {
            return false;
        }
        TestEqual(FString::Printf(TEXT("%s: complete CRIR state restored"), *ModeLabel),
            NormalizeCRIRLineEndings(RestoredDecompile.CRIRText),
            NormalizeCRIRLineEndings(BaselineDecompile.CRIRText));

        return true;
    };

    // Both failure cases intentionally resolve this nonexistent unit struct. Consume exactly
    // those two stable engine diagnostics so the automation result reflects the assertions below.
    AddExpectedErrorPlain(TEXT("Cannot find struct for path '/Script/PinWright.DoesNotExist'."),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/2);

    const bool bReplaceCompleted = RunFailureCase(ECRIRCompileMode::Replace, TEXT("Replace"));
    const bool bExtendCompleted = RunFailureCase(ECRIRCompileMode::Extend, TEXT("Extend"));
    return bReplaceCompleted && bExtendCompleted;
}
