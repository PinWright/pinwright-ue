// Copyright (c) 2026 Alexander Penkin. MIT License.

// Round-trip tests for the @meta(...) / @flags(...) decorator grammar that round-trips
// FKismetUserDeclaredFunctionMetadata + EFunctionFlags through BPIR. Covers the
// function / override / custom_event / macro entry kinds. Engine event entries are
// excluded from the grammar and are not tested here.

#include "Misc/AutomationTest.h"
#include "BpirGraphTestHelpers.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirParser.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Tunnel.h"
#include "EdGraph/EdGraph.h"

using namespace CompilerTestUtils;

namespace
{
    UK2Node_FunctionEntry* FindFunctionEntryByName(UBlueprint* BP, const FString& FunctionName)
    {
        if (!BP) { return nullptr; }
        for (UEdGraph* Graph : BP->FunctionGraphs)
        {
            if (!Graph || !Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase)) { continue; }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
                {
                    return Entry;
                }
            }
        }
        return nullptr;
    }

    UK2Node_CustomEvent* FindCustomEventByName(UBlueprint* BP, const FString& EventName)
    {
        if (!BP) { return nullptr; }
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            if (!Graph) { continue; }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
                if (Event && Event->CustomFunctionName.ToString().Equals(EventName, ESearchCase::IgnoreCase))
                {
                    return Event;
                }
            }
        }
        return nullptr;
    }

    UK2Node_Tunnel* FindMacroEntryTunnel(UBlueprint* BP, const FString& MacroName)
    {
        if (!BP) { return nullptr; }
        for (UEdGraph* Graph : BP->MacroGraphs)
        {
            if (!Graph || !Graph->GetName().Equals(MacroName, ESearchCase::IgnoreCase)) { continue; }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
                if (Tunnel && Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs)
                {
                    return Tunnel;
                }
            }
        }
        return nullptr;
    }
}

// ----------------------------------------------------------------------------
// 1. Function with Category, Tooltip, Pure, Public — entry node carries all
//    four after compile; round-trip decompile preserves @meta(...) + @flags(...)
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirFunctionMetadataCategoryTooltipPurePublicTest,
    "PinWright.bpir.function_metadata.CategoryTooltipPurePublic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirFunctionMetadataCategoryTooltipPurePublicTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirMetaPurePublicBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) { return false; }

    const FString InputBpir = TEXT(
        "@meta(Category=\"Scoring\", Tooltip=\"Returns the current run score.\")\n"
        "@flags(Public, Pure)\n"
        "entry function ComputeScore() {\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    UK2Node_FunctionEntry* Entry = FindFunctionEntryByName(BP, TEXT("ComputeScore"));
    TestNotNull(TEXT("Function entry node exists"), Entry);
    if (!Entry) { return false; }

    TestEqual(TEXT("Category written to MetaData"), Entry->MetaData.Category.ToString(), FString(TEXT("Scoring")));
    TestEqual(TEXT("Tooltip written to MetaData"), Entry->MetaData.ToolTip.ToString(), FString(TEXT("Returns the current run score.")));
    TestTrue(TEXT("FUNC_BlueprintPure bit set"), (Entry->GetExtraFlags() & FUNC_BlueprintPure) != 0);
    TestTrue(TEXT("FUNC_Public bit set"), (Entry->GetExtraFlags() & FUNC_Public) != 0);
    TestEqual(TEXT("Only Public access bit set"), (int32)(Entry->GetExtraFlags() & FUNC_AccessSpecifiers), (int32)FUNC_Public);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) { return false; }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile output contains @meta(...)"), Output.Contains(TEXT("@meta(")));
    TestTrue(TEXT("Decompile output contains Category=\"Scoring\""), Output.Contains(TEXT("Category=")) && Output.Contains(TEXT("Scoring")));
    TestTrue(TEXT("Decompile output contains @flags(...)"), Output.Contains(TEXT("@flags(")));
    TestTrue(TEXT("Decompile output contains Pure flag identifier"), Output.Contains(TEXT("Pure")));

    return true;
}

// ----------------------------------------------------------------------------
// 2. Custom event with Category + CallInEditor — both decorators round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirFunctionMetadataCustomEventCategoryCallInEditorTest,
    "PinWright.bpir.function_metadata.CustomEventCategoryCallInEditor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirFunctionMetadataCustomEventCategoryCallInEditorTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirMetaCustomEventBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) { return false; }

    const FString InputBpir = TEXT(
        "@meta(Category=\"Debug\")\n"
        "@flags(CallInEditor)\n"
        "entry custom_event RunDebugCheck() {\n"
        "    call PrintString(InString: \"check\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    UK2Node_CustomEvent* Event = FindCustomEventByName(BP, TEXT("RunDebugCheck"));
    TestNotNull(TEXT("Custom event exists"), Event);
    if (!Event) { return false; }

    TestEqual(TEXT("Custom event Category written"), Event->GetUserDefinedMetaData().Category.ToString(), FString(TEXT("Debug")));
    TestTrue(TEXT("Custom event bCallInEditor set"), Event->GetUserDefinedMetaData().bCallInEditor);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) { return false; }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile contains @meta with Debug"), Output.Contains(TEXT("Debug")) && Output.Contains(TEXT("@meta(")));
    TestTrue(TEXT("Decompile contains @flags(CallInEditor)"), Output.Contains(TEXT("CallInEditor")) && Output.Contains(TEXT("@flags(")));

    return true;
}

// ----------------------------------------------------------------------------
// 3. Macro with Tooltip only — @meta survives, @flags absent (macros reject flags)
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirFunctionMetadataMacroTooltipTest,
    "PinWright.bpir.function_metadata.MacroTooltip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirFunctionMetadataMacroTooltipTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirMetaMacroBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) { return false; }

    const FString InputBpir = TEXT(
        "@meta(Tooltip=\"Identity macro for testing.\")\n"
        "entry macro IdentityMacro(int Value) -> (int Out) {\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    UK2Node_Tunnel* EntryTunnel = FindMacroEntryTunnel(BP, TEXT("IdentityMacro"));
    TestNotNull(TEXT("Macro entry tunnel exists"), EntryTunnel);
    if (!EntryTunnel) { return false; }

    TestEqual(TEXT("Macro entry Tooltip written"), EntryTunnel->MetaData.ToolTip.ToString(), FString(TEXT("Identity macro for testing.")));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) { return false; }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile contains @meta with Tooltip"), Output.Contains(TEXT("@meta(")) && Output.Contains(TEXT("Tooltip=")));
    TestFalse(TEXT("Decompile does not contain @flags(...) for macro"), Output.Contains(TEXT("@flags(")));

    return true;
}

// ----------------------------------------------------------------------------
// 4. Function with no decorators stays at engine defaults — negative test that
//    proves compile is a no-op when both bMetaPresent and bFlagsPresent are false
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirFunctionMetadataDefaultsNegativeTest,
    "PinWright.bpir.function_metadata.DefaultsNegative",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirFunctionMetadataDefaultsNegativeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirMetaDefaultsBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) { return false; }

    const FString InputBpir = TEXT(
        "entry function PlainFunc() {\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    UK2Node_FunctionEntry* Entry = FindFunctionEntryByName(BP, TEXT("PlainFunc"));
    TestNotNull(TEXT("Function entry node exists"), Entry);
    if (!Entry) { return false; }

    TestTrue(TEXT("Category empty"), Entry->MetaData.Category.IsEmpty());
    TestTrue(TEXT("Tooltip empty"), Entry->MetaData.ToolTip.IsEmpty());
    TestTrue(TEXT("Keywords empty"), Entry->MetaData.Keywords.IsEmpty());
    TestFalse(TEXT("FUNC_BlueprintPure not set"), (Entry->GetExtraFlags() & FUNC_BlueprintPure) != 0);
    TestFalse(TEXT("FUNC_Const not set"), (Entry->GetExtraFlags() & FUNC_Const) != 0);
    TestFalse(TEXT("FUNC_Exec not set"), (Entry->GetExtraFlags() & FUNC_Exec) != 0);
    TestFalse(TEXT("bCallInEditor stays false"), Entry->MetaData.bCallInEditor);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) { return false; }

    const FString& Output = DecompileResult.BpirText;
    TestFalse(TEXT("Decompile output does not contain @meta(...) for default function"), Output.Contains(TEXT("@meta(")));
    TestFalse(TEXT("Decompile output does not contain @flags(...) for default function"), Output.Contains(TEXT("@flags(")));

    return true;
}

// ----------------------------------------------------------------------------
// 5. Recompile-staleness: compile with @meta(Category=...), then recompile the
//    same function without any decorators. The second compile must clear the
//    stale Category so the node returns to engine defaults — guards against
//    the prior "skip when both decorators absent" early-out that left stale
//    metadata sticky across recompiles.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirFunctionMetadataRecompileClearsStaleFieldsTest,
    "PinWright.bpir.function_metadata.RecompileClearsStaleFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirFunctionMetadataRecompileClearsStaleFieldsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirMetaRecompileStaleBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) { return false; }

    // First compile: function carries @meta(Category=...) + @flags(Pure).
    const FString FirstBpir = TEXT(
        "@meta(Category=\"InitialCategory\", Tooltip=\"first tooltip\")\n"
        "@flags(Pure, CallInEditor)\n"
        "entry function StaleCheck() {\n"
        "}"
    );

    FBpirCompiler Compiler1(BP);
    FCompileResult FirstResult = Compiler1.Compile(FirstBpir, EBpirCompileMode::Replace);
    TestTrue(TEXT("First compile succeeded"), FirstResult.bSuccess);
    if (!FirstResult.bSuccess) { return false; }

    UK2Node_FunctionEntry* EntryAfterFirst = FindFunctionEntryByName(BP, TEXT("StaleCheck"));
    TestNotNull(TEXT("Function entry exists after first compile"), EntryAfterFirst);
    if (!EntryAfterFirst) { return false; }
    TestEqual(TEXT("Category set after first compile"),
        EntryAfterFirst->MetaData.Category.ToString(), FString(TEXT("InitialCategory")));
    TestTrue(TEXT("FUNC_BlueprintPure set after first compile"),
        (EntryAfterFirst->GetExtraFlags() & FUNC_BlueprintPure) != 0);
    TestTrue(TEXT("bCallInEditor set after first compile"), EntryAfterFirst->MetaData.bCallInEditor);

    // Second compile: same function name, decorators stripped. Stale fields
    // (Category, Tooltip, Pure flag, CallInEditor) must be cleared.
    const FString SecondBpir = TEXT(
        "entry function StaleCheck() {\n"
        "}"
    );

    FBpirCompiler Compiler2(BP);
    FCompileResult SecondResult = Compiler2.Compile(SecondBpir, EBpirCompileMode::Replace);
    TestTrue(TEXT("Second compile succeeded"), SecondResult.bSuccess);
    if (!SecondResult.bSuccess) { return false; }

    UK2Node_FunctionEntry* EntryAfterSecond = FindFunctionEntryByName(BP, TEXT("StaleCheck"));
    TestNotNull(TEXT("Function entry exists after second compile"), EntryAfterSecond);
    if (!EntryAfterSecond) { return false; }

    TestTrue(TEXT("Category cleared on recompile"), EntryAfterSecond->MetaData.Category.IsEmpty());
    TestTrue(TEXT("Tooltip cleared on recompile"), EntryAfterSecond->MetaData.ToolTip.IsEmpty());
    TestFalse(TEXT("FUNC_BlueprintPure cleared on recompile"),
        (EntryAfterSecond->GetExtraFlags() & FUNC_BlueprintPure) != 0);
    TestFalse(TEXT("bCallInEditor cleared on recompile"), EntryAfterSecond->MetaData.bCallInEditor);

    return true;
}
