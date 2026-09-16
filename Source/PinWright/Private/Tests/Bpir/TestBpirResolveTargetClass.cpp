// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "Blueprint/UserWidget.h"
#include "Compiler/BpirCompiler.h"

// K2Node_CreateWidget.h lives in UMGEditor/Private/Nodes/ — a private header not
// exported to consumers of UMGEditor.  Use SpawnNode<UK2Node_CreateWidget> only when
// the include is reachable; otherwise fall back to runtime class lookup.
#if __has_include("K2Node_CreateWidget.h")
#include "K2Node_CreateWidget.h"
#define BPIR_HAS_CREATE_WIDGET_HEADER 1
#else
#define BPIR_HAS_CREATE_WIDGET_HEADER 0
#endif


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirResolveTargetClassConstructObjectTest,
    "PinWright.bpir.compiler.resolve_target_class.ConstructObjectFromClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirResolveTargetClassConstructObjectTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    UBlueprint* BP = CreateTransientTestBP(TEXT("ResolveTargetClassBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* Graph = BP->UbergraphPages[0];
    TestNotNull(TEXT("Ubergraph page 0 exists"), Graph);
    if (!Graph) return false;

    // K2Node_CreateWidget.h is a private UMGEditor header — guarded path preferred
    // when reachable, runtime class lookup otherwise (established pattern in this suite).
    UK2Node_ConstructObjectFromClass* Node = nullptr;

#if BPIR_HAS_CREATE_WIDGET_HEADER
    Node = SpawnNode<UK2Node_CreateWidget>(Graph, 0, 0);
#else
    UClass* CreateWidgetClass = FindFirstObjectSafe<UClass>(TEXT("K2Node_CreateWidget"));
    if (!CreateWidgetClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("node-class-unavailable"),
            TEXT("UK2Node_CreateWidget class not found at runtime — test skipped"));
        return true;
    }
    UEdGraphNode* RawNode = NewObject<UEdGraphNode>(Graph, CreateWidgetClass);
    RawNode->CreateNewGuid();
    RawNode->PostPlacedNewNode();
    RawNode->AllocateDefaultPins();
    Graph->AddNode(RawNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    Node = Cast<UK2Node_ConstructObjectFromClass>(RawNode);
#endif

    TestNotNull(TEXT("Node cast to UK2Node_ConstructObjectFromClass succeeds"), Node);
    if (!Node) return false;

    // Set the spawn class via ClassPin->DefaultObject, then ReconstructNode so
    // GetClassToSpawn() sees the value.
    UEdGraphPin* ClassPin = Node->GetClassPin();
    TestNotNull(TEXT("ClassPin exists"), ClassPin);
    if (!ClassPin) return false;

    ClassPin->DefaultObject = UUserWidget::StaticClass();
    Node->ReconstructNode();

    // Find the ReturnValue output pin.
    UEdGraphPin* ReturnPin = Node->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    TestNotNull(TEXT("ReturnValue pin exists"), ReturnPin);
    if (!ReturnPin) return false;

    UClass* Resolved = FBpirCompiler::GetAuthoritativePinClass(ReturnPin);
    TestEqual(TEXT("GetAuthoritativePinClass returns UUserWidget (normal case)"),
        Resolved, UUserWidget::StaticClass());

    // Simulate a stale subcategory — mirrors the real-world CreateWidget path where
    // ReconstructNode doesn't fully propagate specialization.
    ReturnPin->PinType.PinSubCategoryObject = UObject::StaticClass();

    UClass* ResolvedAfterDamage = FBpirCompiler::GetAuthoritativePinClass(ReturnPin);
    TestEqual(
        TEXT("GetAuthoritativePinClass still returns UUserWidget after subcategory corrupted to UObject"),
        ResolvedAfterDamage, UUserWidget::StaticClass());

    return true;
}
