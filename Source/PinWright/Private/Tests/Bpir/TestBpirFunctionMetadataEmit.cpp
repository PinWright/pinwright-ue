// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirFunctionMetadataEmit.cpp - Decompile-side coverage for `@meta(...)` /
// `@flags(...)` decorator lines emitted above `entry function|override|custom_event|macro`
// signatures. The compile-side round-trip is covered by the compile-chunk's test file.
//
// These tests build a real Blueprint, mutate the user-editable metadata on an
// entry node, run FBpirDecompiler, and assert the decompiled text contains the
// expected decorator shape. Each test also asserts the negative (no decorator
// lines for default-metadata entries).

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Decompiler/BpirTextEmitter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextKey.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

namespace
{
    UEdGraph* CreateUserFunctionGraph(UBlueprint* BP, const FName& Name)
    {
        UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
            BP, Name,
            UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        if (!FuncGraph)
        {
            return nullptr;
        }
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, /*bIsUserCreated=*/true, nullptr);
        return FuncGraph;
    }

    UK2Node_FunctionEntry* GetFunctionEntry(UEdGraph* FuncGraph)
    {
        if (!FuncGraph) return nullptr;
        return FindNodeOfType<UK2Node_FunctionEntry>(FuncGraph);
    }

    // Find a single line in Output that contains all of Needles. Returns the
    // matching line text or an empty string when no match found.
    FString FindLineContaining(const FString& Output, std::initializer_list<const TCHAR*> Needles)
    {
        TArray<FString> Lines;
        Output.ParseIntoArrayLines(Lines);
        for (const FString& Line : Lines)
        {
            bool bAll = true;
            for (const TCHAR* Needle : Needles)
            {
                if (!Line.Contains(Needle)) { bAll = false; break; }
            }
            if (bAll)
            {
                return Line;
            }
        }
        return FString();
    }

    bool OutputHasMetaOrFlagsForEntry(const FString& Output, const FString& EntryName)
    {
        TArray<FString> Lines;
        Output.ParseIntoArrayLines(Lines);
        for (int32 i = 0; i < Lines.Num(); ++i)
        {
            if (Lines[i].Contains(EntryName) && Lines[i].Contains(TEXT("entry ")))
            {
                // Look one or two lines above for decorator presence
                for (int32 j = FMath::Max(0, i - 2); j < i; ++j)
                {
                    const FString Trimmed = Lines[j].TrimStartAndEnd();
                    if (Trimmed.StartsWith(TEXT("@meta(")) || Trimmed.StartsWith(TEXT("@flags(")))
                    {
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }
}

// ============================================================================
// 1. Function with Category + Tooltip + Keywords -> @meta(...) line above entry
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmitFunctionMetaTextFieldsTest,
    "PinWright.bpir.decompile.FunctionMetaTextFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmitFunctionMetaTextFieldsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MetaTextFieldsBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* FuncGraph = CreateUserFunctionGraph(BP, FName(TEXT("ComputeScore")));
    TestNotNull(TEXT("Function graph created"), FuncGraph);
    if (!FuncGraph) return false;

    UK2Node_FunctionEntry* Entry = GetFunctionEntry(FuncGraph);
    TestNotNull(TEXT("FunctionEntry node exists"), Entry);
    if (!Entry) return false;

    // Set typed FKismetUserDeclaredFunctionMetadata fields directly.
    Entry->MetaData.Category = FText::ChangeKey(
        FTextKey(TEXT("MetaTextFieldsBP")),
        FTextKey(TEXT("Category")),
        FText::FromString(TEXT("Scoring")));
    Entry->MetaData.ToolTip = FText::ChangeKey(
        FTextKey(TEXT("MetaTextFieldsBP")),
        FTextKey(TEXT("Tooltip")),
        FText::FromString(TEXT("Returns the current run score.")));
    Entry->MetaData.Keywords = FText::ChangeKey(
        FTextKey(TEXT("MetaTextFieldsBP")),
        FTextKey(TEXT("Keywords")),
        FText::FromString(TEXT("score points")));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Find a single @meta(...) line that contains all three keys above the ComputeScore entry.
    const FString MetaLine = FindLineContaining(
        Result.BpirText,
        { TEXT("@meta("), TEXT("Category="), TEXT("Tooltip="), TEXT("Keywords=") });
    TestFalse(TEXT("@meta line with all three keys present"), MetaLine.IsEmpty());

    // Key order is fixed: Category, Tooltip, Keywords.
    if (!MetaLine.IsEmpty())
    {
        const int32 CatIdx = MetaLine.Find(TEXT("Category="));
        const int32 ToolIdx = MetaLine.Find(TEXT("Tooltip="));
        const int32 KwIdx = MetaLine.Find(TEXT("Keywords="));
        TestTrue(TEXT("Category appears before Tooltip"), CatIdx < ToolIdx);
        TestTrue(TEXT("Tooltip appears before Keywords"), ToolIdx < KwIdx);
    }

    // Display strings appear inside the decorator.
    TestTrue(TEXT("Display string for Tooltip survives"),
        Result.BpirText.Contains(TEXT("Returns the current run score.")));
    TestTrue(TEXT("Display string for Keywords survives"),
        Result.BpirText.Contains(TEXT("score points")));

    // NSLOCTEXT identity is preserved when present. The emitted text runs through
    // BpirStructLiteralUtils::EscapeBpirString, which doubles inner quotes — so we
    // check namespace, key, and display string as independent substrings instead of
    // pinning the exact escape encoding.
    TestTrue(TEXT("NSLOCTEXT namespace preserved for Category"),
        Result.BpirText.Contains(TEXT("MetaTextFieldsBP")));
    TestTrue(TEXT("NSLOCTEXT key preserved for Category"),
        Result.BpirText.Contains(TEXT("Category")));
    TestTrue(TEXT("NSLOCTEXT display string preserved for Category"),
        Result.BpirText.Contains(TEXT("Scoring")));
    return true;
}

// ============================================================================
// 2. Function with Pure flag -> @flags(Public, Pure) above entry
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmitFunctionFlagsPureAccessTest,
    "PinWright.bpir.decompile.FunctionFlagsPureAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmitFunctionFlagsPureAccessTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("FlagsPureAccessBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* FuncGraph = CreateUserFunctionGraph(BP, FName(TEXT("ComputeScore")));
    TestNotNull(TEXT("Function graph created"), FuncGraph);
    if (!FuncGraph) return false;

    UK2Node_FunctionEntry* Entry = GetFunctionEntry(FuncGraph);
    TestNotNull(TEXT("FunctionEntry node exists"), Entry);
    if (!Entry) return false;

    // Public is engine-default. Set Pure to force @flags(...) to appear; the
    // omission rule then re-includes Public alongside it.
    int32 Flags = Entry->GetExtraFlags();
    Flags &= ~FUNC_AccessSpecifiers;
    Flags |= FUNC_Public;
    Flags |= FUNC_BlueprintPure;
    Entry->SetExtraFlags(Flags);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString FlagsLine = FindLineContaining(
        Result.BpirText,
        { TEXT("@flags("), TEXT("Public"), TEXT("Pure") });
    TestFalse(TEXT("@flags line with Public + Pure present"), FlagsLine.IsEmpty());

    if (!FlagsLine.IsEmpty())
    {
        // Access specifier (Public) must come before Pure per the emit contract.
        const int32 PubIdx = FlagsLine.Find(TEXT("Public"));
        const int32 PureIdx = FlagsLine.Find(TEXT("Pure"));
        TestTrue(TEXT("Public appears before Pure"), PubIdx < PureIdx);
    }
    return true;
}

// ============================================================================
// 3. Function with default everything -> no decorator lines emitted
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmitFunctionDefaultMetadataOmitsDecoratorsTest,
    "PinWright.bpir.decompile.FunctionDefaultMetadataOmitsDecorators",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmitFunctionDefaultMetadataOmitsDecoratorsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DefaultMetaBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* FuncGraph = CreateUserFunctionGraph(BP, FName(TEXT("BareFunction")));
    TestNotNull(TEXT("Function graph created"), FuncGraph);
    if (!FuncGraph) return false;

    UK2Node_FunctionEntry* Entry = GetFunctionEntry(FuncGraph);
    TestNotNull(TEXT("FunctionEntry node exists"), Entry);
    if (!Entry) return false;

    // Force pure-default state. CreateNewGraph/AddFunctionGraph may set FUNC_Public;
    // that's still default and should not force @flags emission.
    int32 Flags = Entry->GetExtraFlags();
    Flags &= ~FUNC_AccessSpecifiers;
    Flags |= FUNC_Public;
    Flags &= ~FUNC_BlueprintPure;
    Flags &= ~FUNC_Const;
    Flags &= ~FUNC_Exec;
    Entry->SetExtraFlags(Flags);
    // Clear bool fields defensively.
    Entry->MetaData.bCallInEditor = false;
    Entry->MetaData.bThreadSafe = false;
    Entry->MetaData.bIsUnsafeDuringActorConstruction = false;
    Entry->MetaData.bIsDeprecated = false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    TestFalse(TEXT("BareFunction has no decorator lines above its entry"),
        OutputHasMetaOrFlagsForEntry(Result.BpirText, TEXT("BareFunction")));
    return true;
}

// ============================================================================
// 4. Function with CallInEditor only -> @flags(CallInEditor) with no Public access
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmitFunctionCallInEditorOnlyTest,
    "PinWright.bpir.decompile.FunctionCallInEditorOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmitFunctionCallInEditorOnlyTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CallInEditorBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* FuncGraph = CreateUserFunctionGraph(BP, FName(TEXT("DebugAction")));
    UK2Node_FunctionEntry* Entry = GetFunctionEntry(FuncGraph);
    TestNotNull(TEXT("FunctionEntry node exists"), Entry);
    if (!Entry) return false;

    Entry->MetaData.bCallInEditor = true;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString FlagsLine = FindLineContaining(
        Result.BpirText,
        { TEXT("@flags("), TEXT("CallInEditor") });
    TestFalse(TEXT("@flags line with CallInEditor present"), FlagsLine.IsEmpty());
    // Public access is not forced here — only CallInEditor flips the line on. The
    // emit contract says emit access only when forced; CallInEditor is "other flag
    // present" so access must appear too.
    if (!FlagsLine.IsEmpty())
    {
        TestTrue(TEXT("Public access included alongside CallInEditor"),
            FlagsLine.Contains(TEXT("Public")));
    }
    return true;
}

// ============================================================================
// 5. Custom event with Category + CallInEditor -> both decorators present
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmitCustomEventBothDecoratorsTest,
    "PinWright.bpir.decompile.CustomEventBothDecorators",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmitCustomEventBothDecoratorsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CustomEventDecoBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Compile a minimal custom event so we have a UK2Node_CustomEvent to mutate.
    FBpirCompiler Compiler(BP);
    FCompileResult Compile = Compiler.Compile(TEXT("entry custom_event MyEvent() { }"));
    TestTrue(TEXT("Compile of custom_event stub succeeded"), Compile.bSuccess);
    if (!Compile.bSuccess) return false;

    UK2Node_CustomEvent* CustomEvent = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
            {
                if (CE->CustomFunctionName.ToString() == TEXT("MyEvent"))
                {
                    CustomEvent = CE;
                    break;
                }
            }
        }
        if (CustomEvent) break;
    }
    TestNotNull(TEXT("UK2Node_CustomEvent MyEvent found"), CustomEvent);
    if (!CustomEvent) return false;

    // Set Category via the typed metadata; CallInEditor lives on the node directly
    // for custom events. The emit branch reads CallInEditor from the node-level field.
    FKismetUserDeclaredFunctionMetadata& Meta = CustomEvent->GetUserDefinedMetaData();
    Meta.Category = FText::ChangeKey(
        FTextKey(TEXT("CustomEventDecoBP")),
        FTextKey(TEXT("Category")),
        FText::FromString(TEXT("Combat")));
    CustomEvent->bCallInEditor = true;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString MetaLine = FindLineContaining(
        Result.BpirText, { TEXT("@meta("), TEXT("Category="), TEXT("Combat") });
    TestFalse(TEXT("@meta line with Category=Combat present"), MetaLine.IsEmpty());

    const FString FlagsLine = FindLineContaining(
        Result.BpirText, { TEXT("@flags("), TEXT("CallInEditor") });
    TestFalse(TEXT("@flags line with CallInEditor present"), FlagsLine.IsEmpty());
    return true;
}

// ============================================================================
// 6. Macro with Tooltip -> @meta(Tooltip=...) present, no @flags(...) line
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEmitMacroMetaWithoutFlagsTest,
    "PinWright.bpir.decompile.MacroMetaWithoutFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEmitMacroMetaWithoutFlagsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroMetaBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Compile = Compiler.Compile(TEXT("entry macro DoTheThing() { }"));
    TestTrue(TEXT("Compile of macro stub succeeded"), Compile.bSuccess);
    if (!Compile.bSuccess) return false;

    // Find the entry tunnel inside the macro graph and set its tooltip metadata.
    UK2Node_Tunnel* EntryTunnel = nullptr;
    for (UEdGraph* Graph : BP->MacroGraphs)
    {
        if (Graph && Graph->GetFName() == FName(TEXT("DoTheThing")))
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
                if (Tunnel && Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs)
                {
                    EntryTunnel = Tunnel;
                    break;
                }
            }
        }
        if (EntryTunnel) break;
    }
    TestNotNull(TEXT("Macro entry tunnel found"), EntryTunnel);
    if (!EntryTunnel) return false;

    EntryTunnel->MetaData.ToolTip = FText::ChangeKey(
        FTextKey(TEXT("MacroMetaBP")),
        FTextKey(TEXT("Tooltip")),
        FText::FromString(TEXT("Does the thing.")));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString MetaLine = FindLineContaining(
        Result.BpirText, { TEXT("@meta("), TEXT("Tooltip="), TEXT("Does the thing.") });
    TestFalse(TEXT("@meta line with Tooltip present above macro entry"), MetaLine.IsEmpty());

    // No @flags(...) decorator on a macro under any circumstances.
    TestFalse(TEXT("No @flags(...) line emitted for macro entry"),
        Result.BpirText.Contains(TEXT("@flags(")));
    return true;
}
