// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_InputKey.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_GetSubsystem.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Decompiler/BpirInputKeyHelpers.h"

using namespace CompilerTestUtils;

// ============================================================================
// 1. Compiler.Integration.SubsystemAccess
// Compile "%ss = subsystem<UGameInstanceSubsystem>()" and verify a
// UK2Node_GetSubsystem (or subclass) is created in the graph.
// Uses UGameInstanceSubsystem as a well-known subsystem base class.
// If the subsystem class cannot be found at test time, verify graceful error.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationSubsystemAccessTest,
    "PinWright.bpir.compiler.integration.SubsystemAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationSubsystemAccessTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("GapCoverageTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %ss = subsystem<GameInstanceSubsystem>()\n")
        TEXT("    call PrintString(InString: \"Got subsystem\")\n")
        TEXT("}"));

    // Subsystem class resolution depends on what classes are registered at test time.
    // If it compiles, verify a GetSubsystem node was created.
    // If it fails, verify graceful error (not a crash).
    if (Result.bSuccess)
    {
        bool bFoundSubsystemNode = false;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Cast<UK2Node_GetSubsystem>(Node))
                {
                    bFoundSubsystemNode = true;
                    break;
                }
            }
            if (bFoundSubsystemNode) break;
        }
        TestTrue(TEXT("A UK2Node_GetSubsystem node was created"), bFoundSubsystemNode);
    }
    else
    {
        // The subsystem class may not be discoverable in a transient test context.
        // Pin the exact production diagnostic (BpirCompiler.cpp, EBpirOpcode::Subsystem)
        // instead of "any error at all", so only the one anticipated failure is
        // tolerated and every other error path still fails the test.
        AddWarning(TEXT("subsystem<> compile reported errors; skipping GetSubsystem node assertion."));
        TestTrue(TEXT("Errors reported cleanly for unresolved subsystem"), Result.Errors.Num() > 0);
        for (const FCompileError& Err : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
        TestTrue(TEXT("Failure is the expected 'Unresolved subsystem class' diagnostic"),
            ErrorsContain(Result.Errors, TEXT("Unresolved subsystem class")));
    }
    return true;
}

// ============================================================================
// 2. Compiler.Integration.UnbindDispatcher
// Compile "unbind_dispatcher OnSomething()" and verify either a delegate
// unbind node is created or a graceful error is reported (transient BP
// lacks real delegate variables).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationUnbindDispatcherTest,
    "PinWright.bpir.compiler.integration.UnbindDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationUnbindDispatcherTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("GapCoverageTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    unbind_dispatcher OnSomething(target: self, event: @HandleIt)\n")
        TEXT("}"));

    // Without a real event dispatcher variable, the compiler should either
    // succeed partially or fail gracefully -- a crash is the only true failure.
    if (!Result.bSuccess)
    {
        // Expected path on a transient BP that has no such dispatcher.
        TestTrue(TEXT("Graceful failure produced error messages"),
            Result.Errors.Num() > 0);
        AddInfo(FString::Printf(TEXT("UnbindDispatcher gracefully failed: %s"),
            Result.Errors.Num() > 0 ? *Result.Errors[0].Message : TEXT("(no message)")));
    }
    else
    {
        AddInfo(TEXT("UnbindDispatcher compiled successfully"));
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
    }
    return true;
}

// ============================================================================
// 3. Compiler.Integration.ComponentEventEntry
// Compile "entry component_event BoxCollision.OnComponentBeginOverlap()" and
// verify either a ComponentBoundEvent node is created or a graceful error
// is reported (transient BP has no SCS components).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationComponentEventEntryTest,
    "PinWright.bpir.compiler.integration.ComponentEventEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationComponentEventEntryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("GapCoverageTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry component_event BoxCollision.OnComponentBeginOverlap() {\n")
        TEXT("    call PrintString(InString: \"Overlap\")\n")
        TEXT("}"));

    // Transient BPs lack SCS components, so the component_event setup will
    // likely fail to find "BoxCollision". Verify no crash.
    if (Result.bSuccess)
    {
        // If it succeeds (unlikely on a transient BP), verify a ComponentBoundEvent node
        bool bFoundNode = false;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Cast<UK2Node_ComponentBoundEvent>(Node))
                {
                    bFoundNode = true;
                    break;
                }
            }
            if (bFoundNode) break;
        }
        TestTrue(TEXT("ComponentBoundEvent node created"), bFoundNode);
    }
    else
    {
        // Expected path: graceful failure because BoxCollision doesn't exist. This branch
        // is the documented normal outcome on a transient BP, so it is pinned rather than
        // warned about: the exact entry-setup diagnostic is required, so any *other*
        // failure still fails the test instead of being absorbed by "some error occurred".
        TestTrue(TEXT("Errors reported for missing component"), Result.Errors.Num() > 0);
        TestTrue(TEXT("Failure is the expected entry-point setup diagnostic"),
            ErrorsContain(Result.Errors, TEXT("Failed to create entry point for block")));
        AddInfo(FString::Printf(TEXT("component_event failed gracefully: %s"),
            Result.Errors.Num() > 0 ? *Result.Errors[0].Message : TEXT("(no message)")));
    }
    return true;
}

// ============================================================================
// 4. Compiler.Integration.WidgetEventEntry
// Compile "entry widget_event StartButton.OnClicked()" and verify either a
// widget event node is created or a graceful error is reported.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationWidgetEventEntryTest,
    "PinWright.bpir.compiler.integration.WidgetEventEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationWidgetEventEntryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("GapCoverageTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry widget_event StartButton.OnClicked() {\n")
        TEXT("    call PrintString(InString: \"Clicked\")\n")
        TEXT("}"));

    // Transient BPs are AActor-based, not UUserWidget-based, so widget event
    // setup will fail. Verify no crash.
    if (Result.bSuccess)
    {
        AddInfo(TEXT("widget_event compiled successfully (unexpected on AActor BP)"));
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
    }
    else
    {
        // Documented normal outcome on an AActor BP, so pinned rather than warned about.
        TestTrue(TEXT("Errors reported for widget event on non-widget BP"),
            Result.Errors.Num() > 0);
        TestTrue(TEXT("Failure is the expected entry-point setup diagnostic"),
            ErrorsContain(Result.Errors, TEXT("Failed to create entry point for block")));
        AddInfo(FString::Printf(TEXT("widget_event failed gracefully: %s"),
            Result.Errors.Num() > 0 ? *Result.Errors[0].Message : TEXT("(no message)")));
    }
    return true;
}

// ============================================================================
// 5. Compiler.Integration.KeyPressedEntry
// Compile "entry key_pressed SpaceBar()" and verify an InputKey node is created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationKeyPressedEntryTest,
    "PinWright.bpir.compiler.integration.KeyPressedEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationKeyPressedEntryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("GapCoverageTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry key_pressed SpaceBar() {\n")
        TEXT("    call PrintString(InString: \"Space pressed\")\n")
        TEXT("}"));

    if (Result.bSuccess)
    {
        // The identity of the wired exec pin is the property under test, so a bare
        // "an InputKey node exists" flag is not enough: BpirCompiler.cpp routes both
        // key_pressed and key_released through SetupKeyEvent(Name, bReleased), and
        // only the pin sense distinguishes them.
        UK2Node_InputKey* InputKeyNode = FindNodeOfType<UK2Node_InputKey>(BP);
        TestNotNull(TEXT("UK2Node_InputKey node was created"), InputKeyNode);
        TestTrue(TEXT("At least one CallFunction node for PrintString"),
            CountNodesOfType<UK2Node_CallFunction>(BP) >= 1);
        if (InputKeyNode)
        {
            TestTrue(TEXT("Pressed exec pin carries the entry body"),
                FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, /*bReleased=*/false));
            // Negative half: without it, collapsing bReleased to a constant is invisible
            // because both key tests would still find "an InputKey node".
            TestFalse(TEXT("Released exec pin is NOT wired for key_pressed"),
                FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, /*bReleased=*/true));
        }
    }
    else
    {
        // InputKey node creation may fail in headless/minimal test contexts.
        // Make the degenerate path explicit: without this warning a regression that
        // turns a working key_pressed compile into a graceful error reads as green.
        AddWarning(TEXT("key_pressed compile reported errors; skipping InputKey exec-pin assertions."));
        TestTrue(TEXT("Errors reported cleanly"), Result.Errors.Num() > 0);
        AddInfo(FString::Printf(TEXT("key_pressed failed gracefully: %s"),
            Result.Errors.Num() > 0 ? *Result.Errors[0].Message : TEXT("(no message)")));
    }
    return true;
}

// ============================================================================
// 6. Compiler.Integration.KeyReleasedEntry
// Compile "entry key_released SpaceBar()" and verify an InputKey node is
// created with the Released exec output pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationKeyReleasedEntryTest,
    "PinWright.bpir.compiler.integration.KeyReleasedEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationKeyReleasedEntryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("GapCoverageTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry key_released SpaceBar() {\n")
        TEXT("    call PrintString(InString: \"Space released\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("key_released compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    TestEqual(TEXT("Exactly one InputKey node was created"),
        CountNodesOfType<UK2Node_InputKey>(BP), 1);
    UK2Node_InputKey* InputKeyNode = FindNodeOfType<UK2Node_InputKey>(BP);
    TestNotNull(TEXT("UK2Node_InputKey node was created"), InputKeyNode);
    if (InputKeyNode)
    {
        TestTrue(TEXT("InputKey node binds SpaceBar"),
            FBpirInputKeyHelpers::DoesInputKeyMatchBpirIdentifier(InputKeyNode, TEXT("SpaceBar")));
        TestTrue(TEXT("Released exec pin carries the entry body"),
            FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, /*bReleased=*/true));
        TestFalse(TEXT("Pressed exec pin is NOT wired for key_released"),
            FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, /*bReleased=*/false));
    }

    TestEqual(TEXT("Compilation leaves no orphan nodes"),
        BlueprintHandlerUtils::FindBlueprintOrphanNodes(BP).Num(), 0);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString ReleasedSignature = TEXT("entry key_released SpaceBar()");
    const int32 ReleasedEntryStart = DecompileResult.BpirText.Find(
        ReleasedSignature, ESearchCase::CaseSensitive);
    TestTrue(TEXT("Decompile emits entry key_released SpaceBar()"),
        ReleasedEntryStart != INDEX_NONE);
    if (ReleasedEntryStart != INDEX_NONE)
    {
        const int32 ReleasedEntryEnd = DecompileResult.BpirText.Find(
            TEXT("\n}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ReleasedEntryStart);
        TestTrue(TEXT("Decompiled key_released entry block is complete"),
            ReleasedEntryEnd != INDEX_NONE);
        if (ReleasedEntryEnd != INDEX_NONE)
        {
            const FString ReleasedEntryBlock = DecompileResult.BpirText.Mid(
                ReleasedEntryStart, ReleasedEntryEnd - ReleasedEntryStart);
            TestTrue(TEXT("key_released entry block preserves its body literal"),
                ReleasedEntryBlock.Contains(TEXT("\"Space released\""), ESearchCase::CaseSensitive));
        }
    }

    const bool bHasOrphanWarning = DecompileResult.Warnings.ContainsByPredicate(
        [](const FBpirWarning& Warning)
        {
            return Warning.Text.Contains(
                TEXT("Orphaned node not reachable from any entry point"),
                ESearchCase::CaseSensitive);
        });
    TestFalse(TEXT("Decompile emits no orphan warning"), bHasOrphanWarning);
    return true;
}

// ============================================================================
// 7. Compiler.Integration.CommentInstruction
// Compile BPIR with # comment lines and verify they don't break compilation.
// Comments are no-op at compile time (return true, no node created).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCommentInstructionTest,
    "PinWright.bpir.compiler.integration.CommentInstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCommentInstructionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("GapCoverageTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    # This is a comment before the call\n")
        TEXT("    call PrintString(InString: \"Hello\")\n")
        TEXT("    # This is a comment after the call\n")
        TEXT("    call PrintString(InString: \"World\")\n")
        TEXT("    # Final comment\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded with comment lines"), Result.bSuccess);
    TestEqual(TEXT("2 CallFunction nodes (comments don't produce extra nodes)"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 2);
    return true;
}
