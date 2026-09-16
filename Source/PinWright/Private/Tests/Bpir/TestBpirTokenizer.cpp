// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirTokenizer.cpp - Unit tests for FBpirTokenizer (lexical tokenization, no UE graph dependencies)

#include "Misc/AutomationTest.h"
#include "Compiler/BpirGrammar.h"
#include "IrCore/IrToken.h"
#include "IrCore/IrTokenizer.h"

// ============================================================================
// 1. KeywordTokenization
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerKeywordTest,
    "PinWright.bpir.tokenizer.KeywordTokenization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerKeywordTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("call MyFunc()"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 4);

    if (Tokens.Num() >= 4)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("call"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("MyFunc"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::OpenParen);
        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::CloseParen);
    }

    return true;
}

// ============================================================================
// 2. KeywordBoundary — "sequenceofevents" is an identifier, not a keyword
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerKeywordBoundaryTest,
    "PinWright.bpir.tokenizer.KeywordBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerKeywordBoundaryTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("sequenceofevents"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 1);

    if (Tokens.Num() >= 1)
    {
        TestEqual(TEXT("Token 0 type is Identifier (not Keyword)"), Tokens[0].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("sequenceofevents"));
    }

    return true;
}

// ============================================================================
// 3. SigilTokens — %, $, keywords, identifiers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerSigilTokensTest,
    "PinWright.bpir.tokenizer.SigilTokens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerSigilTokensTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("%result = pure GetHealth($player)"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 7);

    if (Tokens.Num() >= 7)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::PercentRef);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("%result"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Equals);

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("pure"));

        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 3 text"), Tokens[3].Text, TEXT("GetHealth"));

        TestEqual(TEXT("Token 4 type"), Tokens[4].Type, EIrTokenType::OpenParen);

        TestEqual(TEXT("Token 5 type"), Tokens[5].Type, EIrTokenType::DollarRef);
        TestEqual(TEXT("Token 5 text"), Tokens[5].Text, TEXT("$player"));

        TestEqual(TEXT("Token 6 type"), Tokens[6].Type, EIrTokenType::CloseParen);
    }

    return true;
}

// ============================================================================
// 4. LabelTokens — @loop: produces LabelDef
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerLabelTokensTest,
    "PinWright.bpir.tokenizer.LabelTokens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerLabelTokensTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("@loop: foreach $arr"), GetBpirGrammar());

    TestTrue(TEXT("At least 3 tokens"), Tokens.Num() >= 3);

    if (Tokens.Num() >= 3)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::LabelDef);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("@loop"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("foreach"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::DollarRef);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("$arr"));
    }

    return true;
}

// ============================================================================
// 5. LabelRef — @loop without colon produces LabelRef
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerLabelRefTest,
    "PinWright.bpir.tokenizer.LabelRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerLabelRefTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("branch -> @loop"), GetBpirGrammar());

    TestTrue(TEXT("At least 3 tokens"), Tokens.Num() >= 3);

    if (Tokens.Num() >= 3)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("branch"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Arrow);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("->"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::LabelRef);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("@loop"));
    }

    return true;
}

// ============================================================================
// 6. StringEscape — escaped quotes inside string literals
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerStringEscapeTest,
    "PinWright.bpir.tokenizer.StringEscape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerStringEscapeTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("call Print(\"hello world\")"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 5);

    if (Tokens.Num() >= 5)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("call"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("Print"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::OpenParen);

        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::StringLiteral);
        TestEqual(TEXT("Token 3 text"), Tokens[3].Text, TEXT("\"hello world\""));

        TestEqual(TEXT("Token 4 type"), Tokens[4].Type, EIrTokenType::CloseParen);
    }

    return true;
}

// ============================================================================
// 7. NumberLiterals — positive integers and negative decimals
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerNumberLiteralsTest,
    "PinWright.bpir.tokenizer.NumberLiterals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerNumberLiteralsTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("pure Add(1, -2.5)"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 7);

    if (Tokens.Num() >= 7)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("pure"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("Add"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::OpenParen);

        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::NumberLiteral);
        TestEqual(TEXT("Token 3 text"), Tokens[3].Text, TEXT("1"));

        TestEqual(TEXT("Token 4 type"), Tokens[4].Type, EIrTokenType::Comma);

        TestEqual(TEXT("Token 5 type"), Tokens[5].Type, EIrTokenType::NumberLiteral);
        TestEqual(TEXT("Token 5 text"), Tokens[5].Text, TEXT("-2.5"));

        TestEqual(TEXT("Token 6 type"), Tokens[6].Type, EIrTokenType::CloseParen);
    }

    return true;
}

// ============================================================================
// 8. CommentToken — trailing comment after code
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerCommentTokenTest,
    "PinWright.bpir.tokenizer.CommentToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerCommentTokenTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("call Foo() # this is a comment"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 5);

    if (Tokens.Num() >= 5)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("call"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("Foo"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::OpenParen);
        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::CloseParen);

        TestEqual(TEXT("Token 4 type"), Tokens[4].Type, EIrTokenType::Comment);
        TestEqual(TEXT("Token 4 text"), Tokens[4].Text, TEXT("# this is a comment"));
    }

    return true;
}

// ============================================================================
// 9. CastAngleBrackets — cast<PlayerController>
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerCastAngleBracketsTest,
    "PinWright.bpir.tokenizer.CastAngleBrackets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerCastAngleBracketsTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("cast<PlayerController>"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 4);

    if (Tokens.Num() >= 4)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("cast"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::OpenAngle);

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("PlayerController"));

        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::CloseAngle);
    }

    return true;
}

// ============================================================================
// 10. EmptyLine — produces no tokens
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerEmptyLineTest,
    "PinWright.bpir.tokenizer.EmptyLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerEmptyLineTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT(""), GetBpirGrammar());

    TestEqual(TEXT("Empty line produces no tokens"), Tokens.Num(), 0);

    return true;
}

// ============================================================================
// 11. DoubleBackslashBeforeQuote — "hello\\" tokenizes correctly
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerDoubleBackslashTest,
    "PinWright.bpir.tokenizer.DoubleBackslashBeforeQuote",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerDoubleBackslashTest::RunTest(const FString& Parameters)
{
    // Input: "hello\\" — the \\ is an escaped backslash, so the " after it is the real closing quote
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("\"hello\\\\\""), GetBpirGrammar());

    TestTrue(TEXT("At least 1 token"), Tokens.Num() >= 1);
    if (Tokens.Num() >= 1)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::StringLiteral);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("\"hello\\\\\""));
    }

    return true;
}

// ============================================================================
// 12. TripleBackslash — "hello\\\"world" tokenizes as one string
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerTripleBackslashTest,
    "PinWright.bpir.tokenizer.TripleBackslash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerTripleBackslashTest::RunTest(const FString& Parameters)
{
    // Input: "hello\\\"world" — \\\", first \\ is escaped backslash, \" is escaped quote, world" ends
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("\"hello\\\\\\\"world\""), GetBpirGrammar());

    TestTrue(TEXT("At least 1 token"), Tokens.Num() >= 1);
    if (Tokens.Num() >= 1)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::StringLiteral);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("\"hello\\\\\\\"world\""));
    }

    return true;
}

// ============================================================================
// 13. BracketTokens — [ and ] produce OpenBracket/CloseBracket
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerBracketTokensTest,
    "PinWright.bpir.tokenizer.BracketTokens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerBracketTokensTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("[true -> @then]"), GetBpirGrammar());

    TestTrue(TEXT("At least 5 tokens"), Tokens.Num() >= 5);

    if (Tokens.Num() >= 5)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::OpenBracket);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("["));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("true"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::Arrow);

        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::LabelRef);
        TestEqual(TEXT("Token 3 text"), Tokens[3].Text, TEXT("@then"));

        TestEqual(TEXT("Token 4 type"), Tokens[4].Type, EIrTokenType::CloseBracket);
        TestEqual(TEXT("Token 4 text"), Tokens[4].Text, TEXT("]"));
    }

    return true;
}

// ============================================================================
// 14. ColonToken — : produces Colon (used in named args)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerColonTokenTest,
    "PinWright.bpir.tokenizer.ColonToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerColonTokenTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("PinName: $val"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 3);

    if (Tokens.Num() >= 3)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("PinName"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Colon);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT(":"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::DollarRef);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("$val"));
    }

    return true;
}

// ============================================================================
// 15. DispatcherKeywords — call_dispatcher, bind_dispatcher, unbind_dispatcher,
//     clear_dispatcher are all recognized as Keywords
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerDispatcherKeywordsTest,
    "PinWright.bpir.tokenizer.DispatcherKeywords",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerDispatcherKeywordsTest::RunTest(const FString& Parameters)
{
    const TCHAR* DispatcherKeywords[] = {
        TEXT("call_dispatcher"), TEXT("bind_dispatcher"),
        TEXT("unbind_dispatcher"), TEXT("clear_dispatcher")
    };

    for (const TCHAR* Kw : DispatcherKeywords)
    {
        FString Input = FString::Printf(TEXT("%s MyEvent()"), Kw);
        TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Input, GetBpirGrammar());

        TestTrue(FString::Printf(TEXT("%s: at least 1 token"), Kw), Tokens.Num() >= 1);
        if (Tokens.Num() >= 1)
        {
            TestEqual(FString::Printf(TEXT("%s: is Keyword"), Kw),
                Tokens[0].Type, EIrTokenType::Keyword);
            TestEqual(FString::Printf(TEXT("%s: text"), Kw),
                Tokens[0].Text, FString(Kw));
        }
    }

    return true;
}

// ============================================================================
// 16. SwitchVariantKeywords — switch_int, switch_string, switch_enum are Keywords
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerSwitchVariantKeywordsTest,
    "PinWright.bpir.tokenizer.SwitchVariantKeywords",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerSwitchVariantKeywordsTest::RunTest(const FString& Parameters)
{
    const TCHAR* SwitchKeywords[] = {
        TEXT("switch_int"), TEXT("switch_string"), TEXT("switch_enum")
    };

    for (const TCHAR* Kw : SwitchKeywords)
    {
        TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(FString(Kw), GetBpirGrammar());

        TestEqual(FString::Printf(TEXT("%s: 1 token"), Kw), Tokens.Num(), 1);
        if (Tokens.Num() >= 1)
        {
            TestEqual(FString::Printf(TEXT("%s: is Keyword"), Kw),
                Tokens[0].Type, EIrTokenType::Keyword);
            TestEqual(FString::Printf(TEXT("%s: text"), Kw),
                Tokens[0].Text, FString(Kw));
        }
    }

    return true;
}

// ============================================================================
// 17. ForeachBreakKeyword — foreach_break is a Keyword
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerForeachBreakKeywordTest,
    "PinWright.bpir.tokenizer.ForeachBreakKeyword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerForeachBreakKeywordTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("foreach_break"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 1);
    if (Tokens.Num() >= 1)
    {
        TestEqual(TEXT("Is Keyword"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Text"), Tokens[0].Text, TEXT("foreach_break"));
    }

    return true;
}

// ============================================================================
// 18. EntryKeyword — entry is a Keyword
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerEntryKeywordTest,
    "PinWright.bpir.tokenizer.EntryKeyword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerEntryKeywordTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("entry event BeginPlay"), GetBpirGrammar());

    TestTrue(TEXT("At least 3 tokens"), Tokens.Num() >= 3);
    if (Tokens.Num() >= 3)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("entry"));

        // "event" is not a keyword, it's an identifier in the tokenizer
        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("event"));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("BeginPlay"));
    }

    return true;
}

// ============================================================================
// 19. SelfKeyword — self is recognized as a Keyword
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerSelfKeywordTest,
    "PinWright.bpir.tokenizer.SelfKeyword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerSelfKeywordTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("self"), GetBpirGrammar());

    TestEqual(TEXT("Token count"), Tokens.Num(), 1);
    if (Tokens.Num() >= 1)
    {
        TestEqual(TEXT("Is Keyword"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Text"), Tokens[0].Text, TEXT("self"));
    }

    return true;
}

// ============================================================================
// 20. MakeArrayKeyword — make_array is recognized as a Keyword
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerMakeArrayKeywordTest,
    "PinWright.bpir.tokenizer.MakeArrayKeyword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerMakeArrayKeywordTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("make_array(\"A\", \"B\")"), GetBpirGrammar());

    TestTrue(TEXT("At least 1 token"), Tokens.Num() >= 1);
    if (Tokens.Num() >= 1)
    {
        TestEqual(TEXT("Is Keyword"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Text"), Tokens[0].Text, TEXT("make_array"));
    }

    return true;
}

// ============================================================================
// 21. SubsystemKeyword — subsystem is a BPIR opcode keyword.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerSubsystemIdentifierTest,
    "PinWright.bpir.tokenizer.SubsystemIdentifier",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerSubsystemIdentifierTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("subsystem<AppMusicSubsystem>()"), GetBpirGrammar());

    TestTrue(TEXT("At least 6 tokens"), Tokens.Num() >= 6);

    if (Tokens.Num() >= 6)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::Keyword);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("subsystem"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::OpenAngle);

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("AppMusicSubsystem"));

        TestEqual(TEXT("Token 3 type"), Tokens[3].Type, EIrTokenType::CloseAngle);
        TestEqual(TEXT("Token 4 type"), Tokens[4].Type, EIrTokenType::OpenParen);
        TestEqual(TEXT("Token 5 type"), Tokens[5].Type, EIrTokenType::CloseParen);
    }

    return true;
}

// ============================================================================
// 22. AssetReferencePath — /Game/Path/To/Asset.Asset starts with / which the
//     tokenizer treats as Unknown. Documents current tokenization behavior.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerAssetReferencePathTest,
    "PinWright.bpir.tokenizer.AssetReferencePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerAssetReferencePathTest::RunTest(const FString& Parameters)
{
    // Asset reference paths start with / which is not a recognized token start.
    // The tokenizer currently produces Unknown tokens for / and . characters.
    // This test documents the current behavior for regression detection.
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("/Game/Path/To/Asset.Asset"), GetBpirGrammar());

    TestTrue(TEXT("Produces tokens"), Tokens.Num() > 0);

    // First token is Unknown '/'
    if (Tokens.Num() >= 1)
    {
        TestEqual(TEXT("Token 0 type is Unknown (/)"), Tokens[0].Type, EIrTokenType::Unknown);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("/"));
    }

    // Verify "Game" is recognized as an Identifier
    if (Tokens.Num() >= 2)
    {
        TestEqual(TEXT("Token 1 type is Identifier"), Tokens[1].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("Game"));
    }

    return true;
}

// ============================================================================
// 23. PercentRefDotPin — %name.Pin tokenizes as PercentRef then separate tokens
//     for the dot and pin name
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerPercentRefDotPinTest,
    "PinWright.bpir.tokenizer.PercentRefDotPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerPercentRefDotPinTest::RunTest(const FString& Parameters)
{
    // %loop.ArrayElement — used to reference a specific output pin of a node
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("%loop.ArrayElement"), GetBpirGrammar());

    TestTrue(TEXT("At least 3 tokens"), Tokens.Num() >= 3);

    if (Tokens.Num() >= 3)
    {
        // %loop stops at the dot since '.' is not alphanumeric or underscore
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::PercentRef);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("%loop"));

        // '.' produces Unknown token
        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Unknown);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("."));

        // "ArrayElement" is an identifier
        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("ArrayElement"));
    }

    return true;
}

// ============================================================================
// 24. DollarRefDotProp — $Param.Property tokenizes as DollarRef then separate
//     tokens for dot and property name (used in external property access)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTokenizerDollarRefDotPropTest,
    "PinWright.bpir.tokenizer.DollarRefDotProp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTokenizerDollarRefDotPropTest::RunTest(const FString& Parameters)
{
    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(TEXT("$Param.Health"), GetBpirGrammar());

    TestTrue(TEXT("At least 3 tokens"), Tokens.Num() >= 3);

    if (Tokens.Num() >= 3)
    {
        TestEqual(TEXT("Token 0 type"), Tokens[0].Type, EIrTokenType::DollarRef);
        TestEqual(TEXT("Token 0 text"), Tokens[0].Text, TEXT("$Param"));

        TestEqual(TEXT("Token 1 type"), Tokens[1].Type, EIrTokenType::Unknown);
        TestEqual(TEXT("Token 1 text"), Tokens[1].Text, TEXT("."));

        TestEqual(TEXT("Token 2 type"), Tokens[2].Type, EIrTokenType::Identifier);
        TestEqual(TEXT("Token 2 text"), Tokens[2].Text, TEXT("Health"));
    }

    return true;
}
