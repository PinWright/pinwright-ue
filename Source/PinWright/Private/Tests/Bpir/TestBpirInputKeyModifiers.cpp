// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirInputKeyModifiers.cpp - UK2Node_InputKey modifier flags (bControl / bAlt / bShift /
// bCommand) in BPIR: `entry key_pressed J(ctrl)`. Before the fix the parser rejected the
// modifier list, the emitter printed Ctrl+J as `J()`, and the upsert identity treated J and
// Ctrl+J as the same entry.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "InputCoreTypes.h"
#include "K2Node_InputKey.h"

using namespace CompilerTestUtils;

namespace
{
    bool ParseKeyEntry(const FString& Code, FBpirEntryBlock& OutBlock, TArray<FCompileError>& OutErrors)
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        const bool bOk = Parser.Parse(Code, Blocks, OutErrors, /*bSkipReferenceValidation=*/ true);
        if (Blocks.Num() > 0)
        {
            OutBlock = Blocks[0];
        }
        return bOk && Blocks.Num() == 1;
    }

    TArray<UK2Node_InputKey*> FindInputKeyNodes(UBlueprint* BP)
    {
        TArray<UK2Node_InputKey*> Nodes;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node))
                {
                    Nodes.Add(InputKeyNode);
                }
            }
        }
        return Nodes;
    }

    UK2Node_InputKey* FindInputKeyNode(UBlueprint* BP, const FKey& Key, const bool bCtrl, const bool bAlt, const bool bShift)
    {
        for (UK2Node_InputKey* Node : FindInputKeyNodes(BP))
        {
            if (Node->InputKey == Key && Node->bControl == bCtrl && Node->bAlt == bAlt
                && Node->bShift == bShift && !Node->bCommand)
            {
                return Node;
            }
        }
        return nullptr;
    }

    // Text of the entry block whose signature line starts with Signature, up to its closing brace.
    FString GetEntryBlockText(const FString& Text, const FString& Signature)
    {
        const int32 Start = Text.Find(Signature + TEXT(" "), ESearchCase::CaseSensitive);
        if (Start == INDEX_NONE)
        {
            return FString();
        }
        const int32 End = Text.Find(TEXT("\n}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
        return End == INDEX_NONE ? FString() : Text.Mid(Start, End - Start);
    }

    bool CompileOrReport(FAutomationTestBase& Test, UBlueprint* BP, const FString& Code,
        const EBpirCompileMode Mode, FCompileResult& OutResult)
    {
        FBpirCompiler Compiler(BP);
        OutResult = Compiler.Compile(Code, Mode);
        for (const FCompileError& Err : OutResult.Errors)
        {
            Test.AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
        return Test.TestTrue(TEXT("BPIR compiles"), OutResult.bSuccess);
    }

    FString DecompileOrReport(FAutomationTestBase& Test, UBlueprint* BP)
    {
        FBpirDecompiler Decompiler(BP);
        const FBpirDecompileResult Result = Decompiler.Decompile();
        Test.TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
        return Result.BpirText;
    }

    const TCHAR* ModifierRoundTripCode = TEXT(
        "entry key_pressed J(ctrl) {\n"
        "    call PrintString(InString: \"ctrl j\")\n"
        "}\n"
        "entry key_released F(shift, alt) {\n"
        "    call PrintString(InString: \"shift alt f\")\n"
        "}\n"
        "entry key_pressed J() {\n"
        "    call PrintString(InString: \"plain j\")\n"
        "}");
}

// Fails unfixed: the parser read `ctrl` as a typed parameter and errored "Invalid parameter format".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyModifiersParseCtrlJTest,
    "PinWright.bpir.input_key_modifiers.ParseCtrlJ",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyModifiersParseCtrlJTest::RunTest(const FString& Parameters)
{
    FBpirEntryBlock Block;
    TArray<FCompileError> Errors;
    TestTrue(TEXT("entry key_pressed J(ctrl) parses"),
        ParseKeyEntry(TEXT("entry key_pressed J(ctrl) {\n    call PrintString(InString: \"x\")\n}"), Block, Errors));
    TestEqual(TEXT("No parse errors"), Errors.Num(), 0);
    TestEqual(TEXT("Kind is KeyPressed"), Block.Kind, EBpirEntryKind::KeyPressed);
    TestEqual(TEXT("Name is the bare key"), Block.Name, FString(TEXT("J")));
    TestTrue(TEXT("ctrl set"), Block.bKeyCtrl);
    TestFalse(TEXT("alt not set"), Block.bKeyAlt);
    TestFalse(TEXT("shift not set"), Block.bKeyShift);
    TestFalse(TEXT("cmd not set"), Block.bKeyCmd);
    TestEqual(TEXT("Modifiers are not parameters"), Block.Params.Num(), 0);
    return true;
}

// Fails unfixed: same parameter-format error; also pins order independence of the list.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyModifiersParseShiftAltFTest,
    "PinWright.bpir.input_key_modifiers.ParseShiftAltF",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyModifiersParseShiftAltFTest::RunTest(const FString& Parameters)
{
    FBpirEntryBlock Block;
    TArray<FCompileError> Errors;
    TestTrue(TEXT("entry key_released F(shift, alt) parses"),
        ParseKeyEntry(TEXT("entry key_released F(shift, alt) {\n    call PrintString(InString: \"x\")\n}"), Block, Errors));
    TestEqual(TEXT("Kind is KeyReleased"), Block.Kind, EBpirEntryKind::KeyReleased);
    TestEqual(TEXT("Name is the bare key"), Block.Name, FString(TEXT("F")));
    TestFalse(TEXT("ctrl not set"), Block.bKeyCtrl);
    TestTrue(TEXT("alt set"), Block.bKeyAlt);
    TestTrue(TEXT("shift set"), Block.bKeyShift);
    TestFalse(TEXT("cmd not set"), Block.bKeyCmd);
    return true;
}

// Guard for the historic form: a plain key keeps no modifiers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyModifiersParsePlainKeyTest,
    "PinWright.bpir.input_key_modifiers.ParsePlainKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyModifiersParsePlainKeyTest::RunTest(const FString& Parameters)
{
    FBpirEntryBlock Block;
    TArray<FCompileError> Errors;
    TestTrue(TEXT("entry key_pressed J() parses"),
        ParseKeyEntry(TEXT("entry key_pressed J() {\n    call PrintString(InString: \"x\")\n}"), Block, Errors));
    TestEqual(TEXT("Name is J"), Block.Name, FString(TEXT("J")));
    TestFalse(TEXT("No modifier set"), Block.bKeyCtrl || Block.bKeyAlt || Block.bKeyShift || Block.bKeyCmd);
    return true;
}

// Fails unfixed: the error was the generic "Invalid parameter format", not a modifier diagnostic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyModifiersUnknownModifierTest,
    "PinWright.bpir.input_key_modifiers.UnknownModifierRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyModifiersUnknownModifierTest::RunTest(const FString& Parameters)
{
    FBpirEntryBlock Block;
    TArray<FCompileError> Errors;
    TestFalse(TEXT("entry key_pressed J(meta) is rejected"),
        ParseKeyEntry(TEXT("entry key_pressed J(meta) {\n    call PrintString(InString: \"x\")\n}"), Block, Errors));
    TestTrue(TEXT("Error names the unknown modifier"), Errors.ContainsByPredicate([](const FCompileError& Err)
    {
        return Err.Message.Contains(TEXT("Unknown key modifier 'meta'"));
    }));
    return true;
}

// Decompiler in isolation: the flags are set on the node directly, not through the compiler.
// Fails unfixed: the emitter printed `entry key_pressed J()` for a Ctrl J node.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyModifiersDecompileTest,
    "PinWright.bpir.input_key_modifiers.DecompileEmitsModifiers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyModifiersDecompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("InputKeyModifiersDecompileBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;

    FCompileResult Result;
    if (!CompileOrReport(*this, BP,
        TEXT("entry key_pressed J() {\n    call PrintString(InString: \"j\")\n}\n")
        TEXT("entry key_released F() {\n    call PrintString(InString: \"f\")\n}"),
        EBpirCompileMode::Default, Result))
    {
        return false;
    }

    UK2Node_InputKey* JNode = FindInputKeyNode(BP, EKeys::J, false, false, false);
    UK2Node_InputKey* FNode = FindInputKeyNode(BP, EKeys::F, false, false, false);
    if (!TestNotNull(TEXT("J node"), JNode) || !TestNotNull(TEXT("F node"), FNode)) return false;
    JNode->bControl = true;
    FNode->bShift = true;
    FNode->bAlt = true;

    const FString Text = DecompileOrReport(*this, BP);
    TestTrue(TEXT("Ctrl J decompiles as J(ctrl)"), Text.Contains(TEXT("entry key_pressed J(ctrl)")));
    TestTrue(TEXT("Shift Alt F decompiles as F(alt, shift)"), Text.Contains(TEXT("entry key_released F(alt, shift)")));
    TestFalse(TEXT("No bare J() left"), Text.Contains(TEXT("entry key_pressed J()")));
    return true;
}

// Compile -> decompile -> replace-compile the decompiled text -> decompile: the flags reach the
// nodes, the text is canonical, and the second decompile is byte-identical to the first,
// including the plain J() entry. Fails unfixed at the parse step, and before that fix the
// duplicate-signature scan warned because J and Ctrl J both counted as `key_pressed J`.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyModifiersRoundTripTest,
    "PinWright.bpir.input_key_modifiers.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyModifiersRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("InputKeyModifiersRoundTripBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;

    FCompileResult Result;
    if (!CompileOrReport(*this, BP, ModifierRoundTripCode, EBpirCompileMode::Default, Result)) return false;
    TestFalse(TEXT("J and Ctrl J are not reported as duplicate entries"),
        Result.Warnings.ContainsByPredicate([](const FString& Warning)
        {
            return Warning.Contains(TEXT("Duplicate input-key entry nodes"));
        }));

    TestEqual(TEXT("Three InputKey nodes"), FindInputKeyNodes(BP).Num(), 3);
    TestNotNull(TEXT("Ctrl J node carries bControl only"), FindInputKeyNode(BP, EKeys::J, true, false, false));
    TestNotNull(TEXT("Shift Alt F node carries bShift and bAlt"), FindInputKeyNode(BP, EKeys::F, false, true, true));
    TestNotNull(TEXT("Plain J node carries no modifier"), FindInputKeyNode(BP, EKeys::J, false, false, false));

    const FString First = DecompileOrReport(*this, BP);
    TestTrue(TEXT("Ctrl J entry keeps its body"),
        GetEntryBlockText(First, TEXT("entry key_pressed J(ctrl)")).Contains(TEXT("\"ctrl j\"")));
    TestTrue(TEXT("Shift Alt F entry is canonical and keeps its body"),
        GetEntryBlockText(First, TEXT("entry key_released F(alt, shift)")).Contains(TEXT("\"shift alt f\"")));
    TestTrue(TEXT("Plain J entry is unchanged and keeps its body"),
        GetEntryBlockText(First, TEXT("entry key_pressed J()")).Contains(TEXT("\"plain j\"")));

    if (!CompileOrReport(*this, BP, First, EBpirCompileMode::Replace, Result)) return false;
    TestEqual(TEXT("Replace recompile keeps exactly three InputKey nodes"), FindInputKeyNodes(BP).Num(), 3);
    const FString Second = DecompileOrReport(*this, BP);
    TestEqual(TEXT("Second decompile is byte-identical to the first"), Second, First);
    return true;
}

// Upsert identity: replacing `J(ctrl)` must leave the plain J entry alone.
// Fails unfixed: the identity was `key_pressed J`, so the replace deleted the plain J subgraph too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyModifiersUpsertIdentityTest,
    "PinWright.bpir.input_key_modifiers.UpsertKeepsPlainKeyDistinct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyModifiersUpsertIdentityTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("InputKeyModifiersUpsertBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;

    FCompileResult Result;
    if (!CompileOrReport(*this, BP, ModifierRoundTripCode, EBpirCompileMode::Default, Result)) return false;
    if (!CompileOrReport(*this, BP,
        TEXT("entry key_pressed J(ctrl) {\n    call PrintString(InString: \"ctrl j second\")\n}"),
        EBpirCompileMode::Replace, Result))
    {
        return false;
    }

    TestEqual(TEXT("Still three InputKey nodes"), FindInputKeyNodes(BP).Num(), 3);
    const FString Text = DecompileOrReport(*this, BP);
    const FString CtrlBlock = GetEntryBlockText(Text, TEXT("entry key_pressed J(ctrl)"));
    TestTrue(TEXT("Ctrl J body was replaced"), CtrlBlock.Contains(TEXT("\"ctrl j second\"")));
    TestFalse(TEXT("Old Ctrl J body is gone"), Text.Contains(TEXT("\"ctrl j\"")));
    TestTrue(TEXT("Plain J entry survived the Ctrl J upsert"),
        GetEntryBlockText(Text, TEXT("entry key_pressed J()")).Contains(TEXT("\"plain j\"")));
    return true;
}
