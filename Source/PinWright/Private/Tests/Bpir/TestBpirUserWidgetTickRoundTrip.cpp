// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-event-name-map-class-blind-userwidget-tick and
// B-bpir-event-tick-userwidget-silent-corrupt.
//
// Pre-fix, FBpirCompiler's EventNameMap unconditionally rewrote "Tick" to
// "ReceiveTick" before override resolution. On a UUserWidget-parented Blueprint
// (where the parent's tick UFunction is literally named "Tick", not
// "ReceiveTick"), this caused two regressions:
//
//   1. `entry override Tick(...)` compiled against the rewritten "ReceiveTick"
//      name failed with "No overridable parent function named 'ReceiveTick'".
//   2. `entry event Tick(...)` in append mode silently created a phantom
//      "ReceiveTick" event node distinct from the existing literal-"Tick"
//      override, corrupting adjacent graph topology and surfacing as SKEL_
//      prefixes on subsequent decompile.
//
// The fixes are class-aware: ResolveOverrideEventName in BpirCompiler tries the
// literal name on ParentClass first and only falls back to the Receive-prefix
// mapping when the parent is an AActor subclass. CreateEventNode hard-fails
// when the parent class has no UFunction matching the requested name, instead
// of silently creating a phantom event node.

#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "WidgetBlueprint.h"

using namespace CompilerTestUtils;

namespace
{
    UWidgetBlueprint* CreateTransientUserWidgetBP(const TCHAR* Prefix)
    {
        return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            UUserWidget::StaticClass(),
            GetTransientPackage(),
            FName(*FString::Printf(TEXT("%s_%d"), Prefix, FMath::Rand())),
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass()));
    }

    bool ContainsEventNodeWithMember(UBlueprint* BP, FName MemberName)
    {
        if (!BP) return false;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
                {
                    if (EventNode->IsA<UK2Node_CustomEvent>()) continue;
                    if (EventNode->EventReference.GetMemberName() == MemberName)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// Test 1: `entry override Tick(...)` on UUserWidget compiles or errors cleanly
// (no exception about a missing "ReceiveTick" UFunction).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirUserWidgetTickOverrideRoundTripTest,
    "PinWright.bpir.compiler.integration.UserWidgetTickOverrideRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirUserWidgetTickOverrideRoundTripTest::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = CreateTransientUserWidgetBP(TEXT("UserWidgetTickOverrideBP"));
    TestNotNull(TEXT("UserWidget blueprint was created"), WBP);
    if (!WBP) return false;

    FBpirCompiler Compiler(WBP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry override Tick(struct<Geometry> MyGeometry, float InDeltaTime) {\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            // Diagnostic must NOT mention "ReceiveTick" — the AActor-shaped
            // rewrite is the bug we are guarding against.
            TestFalse(
                FString::Printf(TEXT("Compile diagnostic mentions ReceiveTick (class-blind name map regression): %s"), *Err.Message),
                Err.Message.Contains(TEXT("ReceiveTick")));
        }
    }
    TestTrue(TEXT("`entry override Tick(...)` compiles cleanly against UUserWidget"),
        Result.bSuccess);
    if (!Result.bSuccess) return false;

    // The override should be created against the literal "Tick" UFunction.
    TestTrue(TEXT("UK2Node_Event with member 'Tick' exists after override compile"),
        ContainsEventNodeWithMember(WBP, FName(TEXT("Tick"))));
    TestFalse(TEXT("No phantom 'ReceiveTick' UK2Node_Event was created on UUserWidget"),
        ContainsEventNodeWithMember(WBP, FName(TEXT("ReceiveTick"))));

    // Decompile of the resulting BP must not emit SKEL_ prefixes for self calls.
    FBpirDecompiler Decompiler(WBP);
    FBpirDecompileResult Decompiled = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Decompiled.bSuccess);
    TestFalse(TEXT("Decompile output contains no SKEL_ class prefix"),
        Decompiled.BpirText.Contains(TEXT("SKEL_")));

    return true;
}

// ---------------------------------------------------------------------------
// Test 2: `entry event Tick(...)` on UUserWidget either reuses the existing
// literal-"Tick" override or errors out — must not create a phantom
// "ReceiveTick" event, and must not corrupt a shared-K2Node custom event body.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirUserWidgetTickEventNoPhantomTest,
    "PinWright.bpir.compiler.integration.UserWidgetTickEventNoPhantom",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirUserWidgetTickEventNoPhantomTest::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = CreateTransientUserWidgetBP(TEXT("UserWidgetTickEventBP"));
    TestNotNull(TEXT("UserWidget blueprint was created"), WBP);
    if (!WBP) return false;

    // Add a member variable so we have a stable target for shared $var refs
    // between the Tick override and a custom event — modelling the shared
    // K2Node topology described in B-bpir-event-tick-userwidget-silent-corrupt.
    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(WBP, TEXT("Counter"), IntType);

    // Plant the existing override + a custom event that both read $Counter.
    {
        FBpirCompiler Setup(WBP);
        FCompileResult SetupResult = Setup.Compile(
            TEXT("entry override Tick(struct<Geometry> MyGeometry, float InDeltaTime) {\n")
            TEXT("    set Counter = $Counter\n")
            TEXT("}\n")
            TEXT("entry custom_event SharedReader() {\n")
            TEXT("    set Counter = $Counter\n")
            TEXT("}"));
        TestTrue(TEXT("Setup compile (override Tick + custom_event SharedReader) succeeded"),
            SetupResult.bSuccess);
        if (!SetupResult.bSuccess) return false;
    }

    // Snapshot relevant pre-state: the SharedReader custom event entry node.
    UK2Node_CustomEvent* SharedReaderBefore = nullptr;
    int32 ExecLinkCountBefore = 0;
    for (UEdGraph* Graph : WBP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
            if (!CE) continue;
            if (CE->CustomFunctionName == TEXT("SharedReader"))
            {
                SharedReaderBefore = CE;
                if (UEdGraphPin* Then = CE->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output))
                {
                    ExecLinkCountBefore = Then->LinkedTo.Num();
                }
                break;
            }
        }
        if (SharedReaderBefore) break;
    }
    TestNotNull(TEXT("Pre-existing SharedReader custom event located"), SharedReaderBefore);
    if (!SharedReaderBefore) return false;

    // Submit `entry event Tick(...)` in default (append) mode. Pre-fix this
    // silently created a phantom UK2Node_Event referencing "ReceiveTick".
    FBpirCompiler Compiler(WBP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event Tick(struct<Geometry> MyGeometry, float InDeltaTime) {\n")
        TEXT("    set Counter = $Counter\n")
        TEXT("}"));

    // Either the compile errors cleanly (atomic rollback path) or it reuses
    // the existing override. Both outcomes are acceptable; the unacceptable
    // outcome is a successful compile that emits a phantom "ReceiveTick" node.
    TestFalse(TEXT("No phantom 'ReceiveTick' UK2Node_Event was created"),
        ContainsEventNodeWithMember(WBP, FName(TEXT("ReceiveTick"))));

    // Even on success, the SharedReader's exec-out link count must be preserved
    // (the corruption vector tracked by B-bpir-event-tick-userwidget-silent-corrupt
    // perturbs shared K2Node exec topology).
    int32 ExecLinkCountAfter = 0;
    if (UEdGraphPin* Then = SharedReaderBefore->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output))
    {
        ExecLinkCountAfter = Then->LinkedTo.Num();
    }
    TestEqual(TEXT("SharedReader custom event exec-out link count is preserved"),
        ExecLinkCountAfter, ExecLinkCountBefore);

    // Decompile of the resulting BP must not emit SKEL_ prefixes.
    FBpirDecompiler Decompiler(WBP);
    FBpirDecompileResult Decompiled = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Decompiled.bSuccess);
    TestFalse(TEXT("Decompile output contains no SKEL_ class prefix"),
        Decompiled.BpirText.Contains(TEXT("SKEL_")));

    return true;
}
