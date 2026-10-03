// Copyright (c) 2026 Alexander Penkin. MIT License.

// Wiki-doc regression tests for the BPIR / blueprint pages:
//
// - E-wiki-bpir-example-uncompilable-and-skel-qualifier-undocumented: the struct make/break
//   example shipped `%result: struct<Vector> = break<HitResult>(%hit.OutHit)` followed by a
//   member read, which cannot compile (break<HitResult> builds a generic Break Struct node with
//   no member pins). The test COMPILES the page's own code block, so the next broken example
//   fails here instead of in a reader's session.
// - E-inspect-notes-phantom-includedecompile: blueprint.inspect Notes told callers to pass
//   `includeDecompile`, a parameter the handler rejects with UNKNOWN_PARAMS.
// - E-decompile-bpir-text-not-verbatim-roundtrip: the "round-trip is semantic, not literal"
//   caveat on blueprint.decompile and bpir.entry-points §1b.
// - E-compile-bpir-no-persistence-field: blueprint.compile_bpir's page must say it does not save.
//
// Every page is rendered through WikiHandler::RenderPage, the same path the gateway serves.

#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"
#include "Tests/Bpir/CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Engine/Blueprint.h"
#include "K2Node_BreakStruct.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace TestBpirWikiExampleDocsHelpers
{
    // Body of the first ``` fenced block in Page, or false when there is none.
    bool ExtractFirstCodeBlock(const FString& Page, FString& OutCode)
    {
        const FString Fence(TEXT("```\n"));
        const int32 Open = Page.Find(Fence, ESearchCase::CaseSensitive);
        if (Open == INDEX_NONE)
        {
            return false;
        }
        const int32 BodyStart = Open + Fence.Len();
        const int32 Close = Page.Find(TEXT("\n```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BodyStart);
        if (Close == INDEX_NONE)
        {
            return false;
        }
        OutCode = Page.Mid(BodyStart, Close - BodyStart);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirStructMakeBreakExampleCompilesTest,
    "PinWright.infra.wiki_handler.Topic.BpirStructMakeBreakExampleCompiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirStructMakeBreakExampleCompilesTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("bpir.examples.struct-make-break"), Text))
    {
        return false;
    }
    TestFalse(TEXT("example no longer breaks a native-break HitResult with break<HitResult>"),
        Text.Contains(TEXT("= break<HitResult>(")));
    TestFalse(TEXT("example no longer annotates a HitResult break as struct<Vector>"),
        Text.Contains(TEXT("struct<Vector> = break")));

    FString Code;
    if (!TestTrue(TEXT("page carries a BPIR code block"),
            TestBpirWikiExampleDocsHelpers::ExtractFirstCodeBlock(Text, Code)))
    {
        return false;
    }

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("WikiStructMakeBreakExampleBP"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return false;
    }
    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(Code);
    for (const FCompileError& Err : Result.Errors)
    {
        AddError(FString::Printf(TEXT("Wiki example L%d: %s"), Err.Line, *Err.Message));
    }
    if (!TestTrue(TEXT("the wiki example compiles verbatim"), Result.bSuccess))
    {
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(BP);
    TestEqual(TEXT("the compiled example Blueprint is UpToDate (no generic-break warning or error)"),
        static_cast<int32>(BP->Status), static_cast<int32>(BS_UpToDate));
    // The only Break Struct node the example may build is the break<Margin> one.
    TestEqual(TEXT("exactly one generic Break Struct node (break<Margin>)"),
        CompilerTestUtils::CountNodesOfType<UK2Node_BreakStruct>(BP), 1);

    FString Instructions;
    if (WikiDocTestHelpers::RenderOrFail(*this, TEXT("bpir.instructions"), Instructions))
    {
        TestFalse(TEXT("bpir.instructions §2.7 no longer teaches break<HitResult>(...)"),
            Instructions.Contains(TEXT("= break<HitResult>(")));
        TestTrue(TEXT("bpir.instructions §2.7 shows the BreakHitResult call form"),
            Instructions.Contains(TEXT("call BreakHitResult(Hit:")));
        TestTrue(TEXT("bpir.instructions says the decompiler emits the generated class, not SKEL_"),
            Instructions.Contains(TEXT("SKEL_BPI_Damageable_C")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInspectNoPhantomIncludeDecompileDocTest,
    "PinWright.infra.wiki_handler.MethodPage.BlueprintInspectNoPhantomIncludeDecompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintInspectNoPhantomIncludeDecompileDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("blueprint.inspect"), Text))
    {
        return false;
    }
    TestFalse(TEXT("blueprint.inspect page does not advertise the undeclared includeDecompile"),
        Text.Contains(TEXT("includeDecompile")));
    TestTrue(TEXT("blueprint.inspect page points BPIR text at blueprint.decompile"),
        Text.Contains(TEXT("blueprint.decompile")));
    TestFalse(TEXT("blueprint.inspect page no longer claims graphs come back as BPIR pseudocode"),
        Text.Contains(TEXT("graphs (as BPIR pseudocode")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDecompileRoundTripSemanticDocTest,
    "PinWright.infra.wiki_handler.MethodPage.BlueprintDecompileRoundTripIsSemantic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintDecompileRoundTripSemanticDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("blueprint.decompile"), Text))
    {
        return false;
    }
    TestTrue(TEXT("blueprint.decompile carries the round-trip-is-semantic subsection"),
        Text.Contains(TEXT("Round-trip is semantic (topology + coordinates), not literal text")));
    TestTrue(TEXT("blueprint.decompile names the regenerated labels"), Text.Contains(TEXT("Labels are regenerated")));
    TestTrue(TEXT("blueprint.decompile names the renumbered SSA temps"), Text.Contains(TEXT("SSA temps are renumbered")));
    TestTrue(TEXT("blueprint.decompile names the canonical branch-arm order"),
        Text.Contains(TEXT("Branch arms are canonically ordered")));
    TestTrue(TEXT("blueprint.decompile points at exec-edge verification"),
        Text.Contains(TEXT("get_graph_connections")));

    FString EntryPoints;
    if (WikiDocTestHelpers::RenderOrFail(*this, TEXT("bpir.entry-points"), EntryPoints))
    {
        TestTrue(TEXT("bpir.entry-points §1b carries the positions-not-literal-text caveat"),
            EntryPoints.Contains(TEXT("positions + topology, not literal text")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileBpirStatesNoSaveDocTest,
    "PinWright.infra.wiki_handler.MethodPage.BlueprintCompileBpirStatesNoSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintCompileBpirStatesNoSaveDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("blueprint.compile_bpir"), Text))
    {
        return false;
    }
    TestTrue(TEXT("compile_bpir page says the verb does not save"), Text.Contains(TEXT("It does not save.")));
    TestTrue(TEXT("compile_bpir page names the pendingFlush field"), Text.Contains(TEXT("pendingFlush")));
    TestTrue(TEXT("compile_bpir page names asset.save as the follow-up"), Text.Contains(TEXT("asset.save")));
    return true;
}

// E-bpir-cast-accessor-spelling-undocumented: §2.3's only cast example was single-word, so the
// multi-word accessor spelling (display name, spaces stripped) was documented nowhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCastAccessorSpellingDocTest,
    "PinWright.infra.wiki_handler.Topic.BpirCastAccessorSpelling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCastAccessorSpellingDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("bpir.instructions"), Text))
    {
        return false;
    }
    TestTrue(TEXT("bpir.instructions shows a multi-word cast accessor, space-stripped"),
        Text.Contains(TEXT("`%c.AsBPIRecoilReceiver`")));
    TestTrue(TEXT("bpir.instructions shows the backtick-quoted spaced form"),
        Text.Contains(TEXT("``%c.`AsBPI Recoil Receiver` ``")));
    TestTrue(TEXT("bpir.instructions says the class name verbatim does not resolve"),
        Text.Contains(TEXT("`%c.AsBPI_RecoilReceiver_C`) does not resolve")));
    TestTrue(TEXT("bpir.instructions says the bare register is the cast result"),
        Text.Contains(TEXT("the bare register `%c` resolves to the cast result")));
    return true;
}
