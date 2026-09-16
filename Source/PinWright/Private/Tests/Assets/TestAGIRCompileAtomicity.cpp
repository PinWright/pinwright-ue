// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-agir-compile-failure-not-atomic. A late emission
// failure must restore both Replace deletions and Extend additions, including
// the target package's pre-call dirty state.

#include "Misc/AutomationTest.h"

#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimLayerInterface.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "TestAGIRFixtures.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
const TCHAR* const AnimLayerInterfaceFixturePath =
    TEXT("/Game/Characters/Heroes/Mannequin/Animations/LinkedLayers/ALI_ItemAnimLayers.ALI_ItemAnimLayers_C");

struct FAGIRGraphState
{
    TWeakObjectPtr<UEdGraph> Graph;
    FName GraphName;
    FGuid GraphGuid;
    TArray<TWeakObjectPtr<UEdGraphNode>> Nodes;
};

struct FAGIRInterfaceState
{
    TWeakObjectPtr<UClass> Interface;
    TArray<FAGIRGraphState> Graphs;
};

struct FAGIRTargetState
{
    FString DecompiledText;
    TArray<FAGIRGraphState> Graphs;
    TArray<FAGIRInterfaceState> ImplementedInterfaces;
    bool bPackageDirty = false;
};

void CaptureGraphState(UEdGraph* Graph, FAGIRGraphState& OutState)
{
    OutState.Graph = Graph;
    if (!Graph)
    {
        return;
    }

    OutState.GraphName = Graph->GetFName();
    OutState.GraphGuid = Graph->GraphGuid;
    OutState.Nodes.Reserve(Graph->Nodes.Num());
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        OutState.Nodes.Add(Node);
    }
}

UAnimGraphNode_Root* FindRootNode(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
        {
            return Root;
        }
    }
    return nullptr;
}

bool CaptureTargetState(
    FAutomationTestBase& Test, UAnimBlueprint* AnimBP, FAGIRTargetState& OutState)
{
    const FAGIRDecompileResult DecompileResult = FAGIRDecompiler(AnimBP).Decompile();
    if (!Test.TestTrue(TEXT("pre-failure target decompiles"), DecompileResult.bSuccess))
    {
        return false;
    }
    OutState.DecompiledText = DecompileResult.AGIRText;
    OutState.bPackageDirty = AnimBP->GetOutermost()->IsDirty();

    TArray<UEdGraph*> Graphs;
    AnimBP->GetAllGraphs(Graphs);
    OutState.Graphs.Reserve(Graphs.Num());
    for (UEdGraph* Graph : Graphs)
    {
        FAGIRGraphState& GraphState = OutState.Graphs.AddDefaulted_GetRef();
        CaptureGraphState(Graph, GraphState);
    }

    OutState.ImplementedInterfaces.Reserve(AnimBP->ImplementedInterfaces.Num());
    for (const FBPInterfaceDescription& InterfaceDescription : AnimBP->ImplementedInterfaces)
    {
        FAGIRInterfaceState& InterfaceState =
            OutState.ImplementedInterfaces.AddDefaulted_GetRef();
        InterfaceState.Interface = InterfaceDescription.Interface.Get();
        InterfaceState.Graphs.Reserve(InterfaceDescription.Graphs.Num());
        for (UEdGraph* Graph : InterfaceDescription.Graphs)
        {
            FAGIRGraphState& GraphState = InterfaceState.Graphs.AddDefaulted_GetRef();
            CaptureGraphState(Graph, GraphState);
        }
    }
    return true;
}

bool HasImplementedInterface(UAnimBlueprint* AnimBP, UClass* InterfaceClass)
{
    if (!AnimBP || !InterfaceClass)
    {
        return false;
    }

    for (const FBPInterfaceDescription& InterfaceDescription : AnimBP->ImplementedInterfaces)
    {
        if (InterfaceDescription.Interface.Get() == InterfaceClass)
        {
            return true;
        }
    }
    return false;
}

bool AssertTargetStateUnchanged(
    FAutomationTestBase& Test, UAnimBlueprint* AnimBP, const FAGIRTargetState& Before)
{
    const FAGIRDecompileResult AfterDecompile = FAGIRDecompiler(AnimBP).Decompile();
    bool bMatches = Test.TestTrue(TEXT("rolled-back target still decompiles"), AfterDecompile.bSuccess);
    bMatches &= Test.TestTrue(TEXT("rolled-back AGIR is byte-identical"),
        AfterDecompile.AGIRText.Equals(Before.DecompiledText, ESearchCase::CaseSensitive));
    bMatches &= Test.TestEqual(TEXT("implemented-interface count restored"),
        AnimBP->ImplementedInterfaces.Num(), Before.ImplementedInterfaces.Num());
    bMatches &= Test.TestEqual(TEXT("package dirty state restored"),
        AnimBP->GetOutermost()->IsDirty(), Before.bPackageDirty);

    const int32 ComparableInterfaceCount = FMath::Min(
        AnimBP->ImplementedInterfaces.Num(), Before.ImplementedInterfaces.Num());
    for (int32 InterfaceIndex = 0; InterfaceIndex < ComparableInterfaceCount; ++InterfaceIndex)
    {
        const FAGIRInterfaceState& BeforeInterface = Before.ImplementedInterfaces[InterfaceIndex];
        const FBPInterfaceDescription& AfterInterface = AnimBP->ImplementedInterfaces[InterfaceIndex];
        bMatches &= Test.TestTrue(
            *FString::Printf(TEXT("interface %d identity restored"), InterfaceIndex),
            AfterInterface.Interface.Get() == BeforeInterface.Interface.Get());
        bMatches &= Test.TestEqual(
            *FString::Printf(TEXT("interface %d graph count restored"), InterfaceIndex),
            AfterInterface.Graphs.Num(), BeforeInterface.Graphs.Num());

        const int32 ComparableInterfaceGraphCount = FMath::Min(
            AfterInterface.Graphs.Num(), BeforeInterface.Graphs.Num());
        for (int32 GraphIndex = 0; GraphIndex < ComparableInterfaceGraphCount; ++GraphIndex)
        {
            const FAGIRGraphState& BeforeGraph = BeforeInterface.Graphs[GraphIndex];
            UEdGraph* const AfterGraph = AfterInterface.Graphs[GraphIndex];
            bMatches &= Test.TestTrue(
                *FString::Printf(TEXT("interface %d graph %d identity restored"),
                    InterfaceIndex, GraphIndex),
                AfterGraph == BeforeGraph.Graph.Get());
            if (!AfterGraph)
            {
                continue;
            }

            bMatches &= Test.TestTrue(
                *FString::Printf(TEXT("interface %d graph %d name restored"),
                    InterfaceIndex, GraphIndex),
                AfterGraph->GetFName() == BeforeGraph.GraphName);
            bMatches &= Test.TestTrue(
                *FString::Printf(TEXT("interface %d graph %d guid restored"),
                    InterfaceIndex, GraphIndex),
                AfterGraph->GraphGuid == BeforeGraph.GraphGuid);
            bMatches &= Test.TestEqual(
                *FString::Printf(TEXT("interface %d graph %d node count restored"),
                    InterfaceIndex, GraphIndex),
                AfterGraph->Nodes.Num(), BeforeGraph.Nodes.Num());
            const int32 ComparableNodeCount = FMath::Min(
                AfterGraph->Nodes.Num(), BeforeGraph.Nodes.Num());
            for (int32 NodeIndex = 0; NodeIndex < ComparableNodeCount; ++NodeIndex)
            {
                bMatches &= Test.TestTrue(
                    *FString::Printf(TEXT("interface %d graph %d node %d identity restored"),
                        InterfaceIndex, GraphIndex, NodeIndex),
                    AfterGraph->Nodes[NodeIndex] == BeforeGraph.Nodes[NodeIndex].Get());
            }
        }
    }

    TArray<UEdGraph*> AfterGraphs;
    AnimBP->GetAllGraphs(AfterGraphs);
    bMatches &= Test.TestEqual(TEXT("graph count restored"), AfterGraphs.Num(), Before.Graphs.Num());
    const int32 ComparableGraphCount = FMath::Min(AfterGraphs.Num(), Before.Graphs.Num());
    for (int32 GraphIndex = 0; GraphIndex < ComparableGraphCount; ++GraphIndex)
    {
        const FAGIRGraphState& BeforeGraph = Before.Graphs[GraphIndex];
        UEdGraph* const AfterGraph = AfterGraphs[GraphIndex];
        bMatches &= Test.TestTrue(
            *FString::Printf(TEXT("graph %d identity restored"), GraphIndex),
            AfterGraph == BeforeGraph.Graph.Get());
        if (!AfterGraph)
        {
            continue;
        }

        bMatches &= Test.TestEqual(
            *FString::Printf(TEXT("graph %d node count restored"), GraphIndex),
            AfterGraph->Nodes.Num(), BeforeGraph.Nodes.Num());
        const int32 ComparableNodeCount = FMath::Min(AfterGraph->Nodes.Num(), BeforeGraph.Nodes.Num());
        for (int32 NodeIndex = 0; NodeIndex < ComparableNodeCount; ++NodeIndex)
        {
            bMatches &= Test.TestTrue(
                *FString::Printf(TEXT("graph %d node %d identity restored"), GraphIndex, NodeIndex),
                AfterGraph->Nodes[NodeIndex] == BeforeGraph.Nodes[NodeIndex].Get());
        }
    }
    return bMatches;
}

bool RunLateFailureAtomicityCase(
    FAutomationTestBase& Test,
    USkeleton* Skeleton,
    UClass* ExistingAnimLayerInterfaceClass,
    UClass* AddedAnimLayerInterfaceClass,
    EAGIRCompileMode Mode,
    const TCHAR* ModeName)
{
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_Atomic%s_%s"),
        ModeName,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(TargetPath, Skeleton);
    if (!Test.TestNotNull(TEXT("scratch AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!Test.TestNotNull(TEXT("scratch AnimGraph present"), AnimGraph))
    {
        return false;
    }

    if (!Test.TestFalse(TEXT("scratch target does not already implement the fixture interface"),
        HasImplementedInterface(AnimBP, ExistingAnimLayerInterfaceClass))
        || !Test.TestFalse(TEXT("scratch target does not already implement the added interface"),
            HasImplementedInterface(AnimBP, AddedAnimLayerInterfaceClass)))
    {
        return false;
    }

    if (!Test.TestTrue(TEXT("pre-existing anim-layer interface implementation created"),
        FBlueprintEditorUtils::ImplementNewInterface(
            AnimBP, ExistingAnimLayerInterfaceClass->GetClassPathName())))
    {
        return false;
    }

    const FBPInterfaceDescription* ExistingInterface = AnimBP->ImplementedInterfaces.FindByPredicate(
        [ExistingAnimLayerInterfaceClass](const FBPInterfaceDescription& Description)
        {
            return Description.Interface.Get() == ExistingAnimLayerInterfaceClass;
        });
    if (!Test.TestNotNull(TEXT("pre-existing interface description present"), ExistingInterface)
        || !Test.TestTrue(TEXT("pre-existing interface graph present"), ExistingInterface->Graphs.Num() > 0))
    {
        return false;
    }

    UAnimGraphNode_Base* InterfaceSeedNode = AnimGraphConstructionUtils::CreateAnimNode(
        ExistingInterface->Graphs[0], UAnimGraphNode_SequencePlayer::StaticClass(),
        FVector2D(-240.0, 40.0));
    if (!Test.TestNotNull(TEXT("pre-existing interface graph node created"), InterfaceSeedNode))
    {
        return false;
    }

    UAnimGraphNode_Base* SeedNode = AnimGraphConstructionUtils::CreateAnimNode(
        AnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(-240.0, 40.0));
    UAnimGraphNode_Root* RootNode = FindRootNode(AnimGraph);
    if (!Test.TestNotNull(TEXT("seed SequencePlayer created"), SeedNode)
        || !Test.TestNotNull(TEXT("seed root present"), RootNode))
    {
        return false;
    }
    if (!Test.TestTrue(TEXT("seed pose link created"),
        AnimGraphConstructionUtils::WirePoseLink(
            SeedNode, FName(TEXT("Pose")), RootNode, FName(TEXT("Result")))))
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    AnimBP->GetOutermost()->SetDirtyFlag(false);

    FAGIRTargetState Before;
    if (!CaptureTargetState(Test, AnimBP, Before))
    {
        return false;
    }

    // The first call emits a real node. The second reaches class resolution only
    // after that mutation and fails, exercising rollback rather than preflight.
    const FString InterfaceClassPath = AddedAnimLayerInterfaceClass->GetClassPathName().ToString();
    const FString BadAGIR = FString::Printf(
        TEXT("interfaces {\n"
             "    implements `%s`\n"
             "}\n"
             "entry anim_graph \"AnimGraph\" {\n"
             "    %%created_before_failure = call `/Script/AnimGraph.AnimGraphNode_SequencePlayer`\n"
             "    %%late_failure = call `/Script/AnimGraph.AnimGraphNode_PinWrightMissingAtomicityTest`\n"
             "    output %%created_before_failure\n"
             "}\n"),
        *InterfaceClassPath);

    FAGIRCompileOptions Options;
    Options.Context = AnimBP->GetPathName();
    Options.Mode = Mode;
    Options.bRunLayout = false;
    Options.bSave = false;

    const FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(BadAGIR, Options);
    Test.TestFalse(TEXT("late emission failure is reported"), CompileResult.bSuccess);
    Test.TestEqual(TEXT("late failure reports AGIR_CLASS_NOT_FOUND"),
        CompileResult.ErrorCode, FString(TEXT("AGIR_CLASS_NOT_FOUND")));
    Test.TestFalse(TEXT("newly requested interface is absent after rollback"),
        HasImplementedInterface(AnimBP, AddedAnimLayerInterfaceClass));
    return AssertTargetStateUnchanged(Test, AnimBP, Before);
}

bool RunInterfaceMutationFailureCase(
    FAutomationTestBase& Test,
    USkeleton* Skeleton,
    UClass* AnimLayerInterfaceClass)
{
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_InterfaceMutation_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(TargetPath, Skeleton);
    if (!Test.TestNotNull(TEXT("scratch AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    FName ConflictingFunctionName;
    for (TFieldIterator<UFunction> FunctionIter(
        AnimLayerInterfaceClass, EFieldIteratorFlags::IncludeSuper); FunctionIter; ++FunctionIter)
    {
        if ((*FunctionIter)->HasMetaData(FBlueprintMetadata::MD_AnimBlueprintFunction))
        {
            ConflictingFunctionName = (*FunctionIter)->GetFName();
            break;
        }
    }
    if (!Test.TestTrue(TEXT("anim-layer fixture has an anim function for collision"),
        !ConflictingFunctionName.IsNone()))
    {
        return false;
    }

    UEdGraph* ConflictingGraph = FBlueprintEditorUtils::CreateNewGraph(
        AnimBP, ConflictingFunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!Test.TestNotNull(TEXT("conflicting interface function graph created"), ConflictingGraph))
    {
        return false;
    }
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(
        AnimBP, ConflictingGraph, /*bIsUserCreated=*/true, nullptr);
    AnimBP->GetOutermost()->SetDirtyFlag(false);

    FAGIRTargetState Before;
    if (!CaptureTargetState(Test, AnimBP, Before))
    {
        return false;
    }

    const FString InterfaceClassPath = AnimLayerInterfaceClass->GetClassPathName().ToString();
    const FString BadAGIR = FString::Printf(
        TEXT("interfaces {\n"
             "    implements `%s`\n"
             "}\n"),
        *InterfaceClassPath);

    FAGIRCompileOptions Options;
    Options.Context = AnimBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    const FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(BadAGIR, Options);
    Test.TestFalse(TEXT("interface mutation failure is reported"), CompileResult.bSuccess);
    Test.TestEqual(TEXT("interface mutation failure reports AGIR_INTERFACE_MUTATION_FAILED"),
        CompileResult.ErrorCode, FString(TEXT("AGIR_INTERFACE_MUTATION_FAILED")));
    return AssertTargetStateUnchanged(Test, AnimBP, Before);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRCompileFailureAtomicReplaceTest,
    "PinWright.anim.agir.CompileFailureAtomicReplace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRCompileFailureAtomicReplaceTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AGIRTestFixtures::LyraMannequinAnimBPPath);
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AnimLayerInterfaceFixturePath);
    UAnimBlueprint* Fixture = AGIRTestFixtures::LoadLyraMannequinAnimBP();
    UClass* AnimLayerInterface = LoadObject<UClass>(nullptr, AnimLayerInterfaceFixturePath);
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)
        || !TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())
        || !TestNotNull(TEXT("AnimLayerInterface fixture loaded"), AnimLayerInterface)
        || !TestTrue(TEXT("AnimLayerInterface fixture derives from UAnimLayerInterface"),
            AnimLayerInterface->IsChildOf(UAnimLayerInterface::StaticClass())))
    {
        return false;
    }
    return RunLateFailureAtomicityCase(
        *this, Fixture->TargetSkeleton.Get(), AnimLayerInterface,
        UAnimLayerInterface::StaticClass(),
        EAGIRCompileMode::Replace, TEXT("Replace"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRCompileFailureAtomicExtendTest,
    "PinWright.anim.agir.CompileFailureAtomicExtend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRCompileFailureAtomicExtendTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AGIRTestFixtures::LyraMannequinAnimBPPath);
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AnimLayerInterfaceFixturePath);
    UAnimBlueprint* Fixture = AGIRTestFixtures::LoadLyraMannequinAnimBP();
    UClass* AnimLayerInterface = LoadObject<UClass>(nullptr, AnimLayerInterfaceFixturePath);
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)
        || !TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())
        || !TestNotNull(TEXT("AnimLayerInterface fixture loaded"), AnimLayerInterface)
        || !TestTrue(TEXT("AnimLayerInterface fixture derives from UAnimLayerInterface"),
            AnimLayerInterface->IsChildOf(UAnimLayerInterface::StaticClass())))
    {
        return false;
    }
    return RunLateFailureAtomicityCase(
        *this, Fixture->TargetSkeleton.Get(), AnimLayerInterface,
        UAnimLayerInterface::StaticClass(),
        EAGIRCompileMode::Extend, TEXT("Extend"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRCompileInterfaceMutationFailureTest,
    "PinWright.anim.agir.CompileInterfaceMutationFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRCompileInterfaceMutationFailureTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AGIRTestFixtures::LyraMannequinAnimBPPath);
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AnimLayerInterfaceFixturePath);
    UAnimBlueprint* Fixture = AGIRTestFixtures::LoadLyraMannequinAnimBP();
    UClass* AnimLayerInterface = LoadObject<UClass>(nullptr, AnimLayerInterfaceFixturePath);
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)
        || !TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())
        || !TestNotNull(TEXT("AnimLayerInterface fixture loaded"), AnimLayerInterface)
        || !TestTrue(TEXT("AnimLayerInterface fixture derives from UAnimLayerInterface"),
            AnimLayerInterface->IsChildOf(UAnimLayerInterface::StaticClass())))
    {
        return false;
    }
    return RunInterfaceMutationFailureCase(*this, Fixture->TargetSkeleton.Get(), AnimLayerInterface);
}
