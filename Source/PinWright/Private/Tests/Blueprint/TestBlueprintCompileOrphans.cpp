// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for E-blueprint-compile-silent-about-orphans: UE compiles a
// disconnected VariableGet cleanly, so blueprint.compile must report it itself, with
// the same orphan model blueprint.graph.find_orphaned_nodes uses.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestUtils.h"
#include "UObject/StrongObjectPtr.h"

namespace TestBlueprintCompileOrphansHelpers
{
    bool CompileAndCapture(FAutomationTestBase& Test, UBlueprint* BP, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BP->GetPathName());
        Test.TestTrue(TEXT("blueprint.compile handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile"), Payload, Capture));
        return Test.TestTrue(TEXT("blueprint.compile succeeded"), Capture.bSuccess && Capture.Result.IsValid());
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileReportsOrphanedNodesTest,
    "PinWright.blueprint.compile.ReportsOrphanedNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCompileReportsOrphanedNodesTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintCompileOrphansHelpers;

    TStrongObjectPtr<UBlueprint> Blueprint(CompilerTestUtils::CreateTransientTestBP(TEXT("CompileOrphans")));
    UBlueprint* BP = Blueprint.Get();
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return true;
    }
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    const FName VarName(TEXT("OrphanProbe"));
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    if (!TestNotNull(TEXT("Event graph exists"), EventGraph)
        || !TestTrue(TEXT("Member variable added"), FBlueprintEditorUtils::AddMemberVariable(BP, VarName, BoolPinType)))
    {
        return true;
    }

    // Baseline: a graph with no orphan reports orphanedCount 0 and no orphanedNodes.
    FTestResponseCapture Clean;
    if (!CompileAndCapture(*this, BP, Clean))
    {
        return true;
    }
    double CleanCount = -1;
    TestTrue(TEXT("Clean compile carries orphanedCount"), Clean.Result->TryGetNumberField(TEXT("orphanedCount"), CleanCount));
    TestEqual(TEXT("Clean graph reports no orphans"), static_cast<int32>(CleanCount), 0);
    TestFalse(TEXT("Clean graph omits orphanedNodes"), Clean.Result->HasField(TEXT("orphanedNodes")));

    // A getter wired to nothing: legal Blueprint, so UE reports no error or warning.
    UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(EventGraph);
    Getter->VariableReference.SetSelfMember(VarName);
    Getter->CreateNewGuid();
    Getter->PostPlacedNewNode();
    Getter->AllocateDefaultPins();
    EventGraph->AddNode(Getter, false, false);

    FTestResponseCapture Orphaned;
    if (!CompileAndCapture(*this, BP, Orphaned))
    {
        return true;
    }
    const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
    TestTrue(TEXT("Precondition: UE compiles the orphan without errors"),
        Orphaned.Result->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() == 0);

    double Count = -1;
    TestTrue(TEXT("Compile carries orphanedCount"), Orphaned.Result->TryGetNumberField(TEXT("orphanedCount"), Count));
    TestEqual(TEXT("Compile counts the disconnected getter"), static_cast<int32>(Count), 1);
    const TSharedPtr<FJsonObject> Row = JsonArrayFindObjectByStringField(
        Orphaned.Result, TEXT("orphanedNodes"), TEXT("nodeId"), Getter->NodeGuid.ToString());
    TestTrue(TEXT("orphanedNodes names the getter by nodeId"), Row.IsValid());
    return true;
}
