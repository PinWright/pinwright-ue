// Copyright (c) 2026 Alexander Penkin. MIT License.

// Guards the four properties that justify FPwTokenizer existing at all rather
// than .pwmodel reusing FIrTokenizer. Reverting any of them reintroduces a defect
// the shared tokenizer has today:
//
//  1. Exponents. FIrTokenizer::ReadNumber (Private/IrCore/IrTokenizer.cpp:54-82)
//     stops before 'e', so `1e-5` lexes as three tokens - the number 1, the
//     identifier e, and the number -5. .pwmodel carries tolerances and real-world
//     dimensions; that split silently changes the value a caller wrote.
//  2. Newlines are tokens. FIrTokenizer takes one pre-split line, so a caller has
//     to reconstruct statement boundaries. Deleting the Newline emissions makes a
//     comment-only line fuse the statements on either side of it.
//  3. Line AND column. FIrToken carries a character Position within its line only.
//     Dropping either coordinate here degrades every PWMODEL_* diagnostic, which is
//     the whole authoring UX for a format meant to be written by an LLM.
//  4. Totality. A lexical error emits an Unknown token spanning the offending run
//     and keeps going. Replacing that with an early return loses every later
//     diagnostic; replacing it with "skip one char and retry" on an unterminated
//     string re-enters the string scanner at each character and turns one bad line
//     into a quadratic pile of diagnostics.
#include "Misc/AutomationTest.h"

// PwModelAst.h is header-only and has no .cpp of its own, so nothing would compile it
// until the parser lands. Included here to keep it compiling with the rest of Model/.
#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "PwSource/PwToken.h"
#include "PwSource/PwTokenizer.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
// helper with a common name would collide with a sibling test TU when Unity merges them.
void PwModelTokenizerTest_ExpectToken(
    FAutomationTestBase& Test,
    const TArray<FPwToken>& Tokens,
    int32 Index,
    EPwTokenType ExpectedType,
    const TCHAR* ExpectedText,
    int32 ExpectedLine,
    int32 ExpectedColumn)
{
    const FString Label = FString::Printf(TEXT("token[%d] (%s '%s')"), Index, PwTokenTypeToString(ExpectedType), ExpectedText);

    if (!Tokens.IsValidIndex(Index))
    {
        Test.AddError(FString::Printf(TEXT("%s missing: only %d tokens were produced"), *Label, Tokens.Num()));
        return;
    }

    const FPwToken& Token = Tokens[Index];
    Test.TestEqual(*(Label + TEXT(" type")), static_cast<int32>(Token.Type), static_cast<int32>(ExpectedType));
    Test.TestEqual(*(Label + TEXT(" text")), Token.Text, FString(ExpectedText));
    Test.TestEqual(*(Label + TEXT(" line")), Token.Line, ExpectedLine);
    Test.TestEqual(*(Label + TEXT(" column")), Token.Column, ExpectedColumn);
}

int32 PwModelTokenizerTest_CountType(const TArray<FPwToken>& Tokens, EPwTokenType Type)
{
    int32 Count = 0;
    for (const FPwToken& Token : Tokens)
    {
        if (Token.Type == Type)
        {
            ++Count;
        }
    }
    return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTokenizerExponentTest,
    "PinWright.Model.Tokenizer.ExponentNumbersLexAsOneToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwTokenizerExponentTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> Diagnostics;

    const bool bOk = FPwTokenizer::Tokenize(
        TEXTVIEW("noise_deform frequency=1e-5 magnitude=2.5E+3 seed=-1e5"), Tokens, Diagnostics);

    TestTrue(TEXT("exponent literals lex without diagnostics"), bOk);
    TestEqual(TEXT("no diagnostics"), Diagnostics.Num(), 0);

    // Three numbers, not nine: under FIrTokenizer's scanner each of these splits into
    // number / identifier / number.
    TestEqual(TEXT("exactly three number tokens"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::Number), 3);

    PwModelTokenizerTest_ExpectToken(*this, Tokens, 3, EPwTokenType::Number, TEXT("1e-5"), 1, 24);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 6, EPwTokenType::Number, TEXT("2.5E+3"), 1, 39);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 9, EPwTokenType::Number, TEXT("-1e5"), 1, 51);

    for (const FPwToken& Token : Tokens)
    {
        if (Token.Type == EPwTokenType::Identifier)
        {
            TestNotEqual(TEXT("no bare exponent marker leaked out as an identifier"), Token.Text, FString(TEXT("e")));
            TestNotEqual(TEXT("no bare exponent marker leaked out as an identifier"), Token.Text, FString(TEXT("E")));
        }
    }

    // An exponent marker with no digits is a diagnostic, not a silent split into the
    // number 1 followed by the identifier e.
    TArray<FPwToken> BadTokens;
    TArray<FPwDiagnostic> BadDiagnostics;
    TestFalse(TEXT("'1e' is rejected"), FPwTokenizer::Tokenize(TEXTVIEW("radius=1e"), BadTokens, BadDiagnostics));
    TestEqual(TEXT("one diagnostic for the malformed exponent"), BadDiagnostics.Num(), 1);
    if (BadDiagnostics.Num() == 1)
    {
        TestEqual(TEXT("malformed exponent code"), BadDiagnostics[0].Code,
            FString(PwSourceDiagnosticCodes::PWSRC_INVALID_NUMBER));
        TestEqual(TEXT("malformed exponent line"), BadDiagnostics[0].Line, 1);
        TestEqual(TEXT("malformed exponent column"), BadDiagnostics[0].Column, 8);
    }
    TestEqual(TEXT("malformed exponent produces no number token"),
        PwModelTokenizerTest_CountType(BadTokens, EPwTokenType::Number), 0);

    // Property 4 in this file's header - totality - for the exponent path specifically. The
    // "no number token" count above is satisfied by a scanner that emits NOTHING for the bad
    // run and resumes past it, which is the shape that silently drops the author's text from
    // the stream. What the tokenizer must do is consume the whole malformed run into ONE
    // Unknown token starting at the run's own column, so the parser resynchronises on real
    // positions rather than on a hole.
    PwModelTokenizerTest_ExpectToken(*this, BadTokens, 2, EPwTokenType::Unknown, TEXT("1e"), 1, 8);
    PwModelTokenizerTest_ExpectToken(*this, BadTokens, 3, EPwTokenType::EndOfFile, TEXT(""), 1, 10);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTokenizerCommentTest,
    "PinWright.Model.Tokenizer.CommentsEmitNothingAndPreserveLineNumbers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwTokenizerCommentTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> Diagnostics;

    const bool bOk = FPwTokenizer::Tokenize(
        TEXTVIEW("# leading comment\npwmodel 0   # trailing comment\n# another\npart body\n"),
        Tokens, Diagnostics);

    TestTrue(TEXT("comments lex without diagnostics"), bOk);

    for (const FPwToken& Token : Tokens)
    {
        TestFalse(TEXT("no comment text survives lexing"), Token.Text.Contains(TEXT("comment")));
        TestFalse(TEXT("no comment sigil survives lexing"), Token.Text.Contains(TEXT("#")));
    }

    // Every line break still produces a token, including the ones that end a
    // comment-only line - otherwise `part` would join the previous statement.
    TestEqual(TEXT("four newline tokens"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::Newline), 4);

    PwModelTokenizerTest_ExpectToken(*this, Tokens, 0, EPwTokenType::Newline, TEXT(""), 1, 18);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 1, EPwTokenType::Identifier, TEXT("pwmodel"), 2, 1);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 2, EPwTokenType::Number, TEXT("0"), 2, 9);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 3, EPwTokenType::Newline, TEXT(""), 2, 31);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 4, EPwTokenType::Newline, TEXT(""), 3, 10);

    // The payload of line 4 keeps its real line number: a comment-only line must not
    // be swallowed, or every diagnostic below it points one line too high.
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 5, EPwTokenType::Identifier, TEXT("part"), 4, 1);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 6, EPwTokenType::Identifier, TEXT("body"), 4, 6);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 7, EPwTokenType::Newline, TEXT(""), 4, 10);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 8, EPwTokenType::EndOfFile, TEXT(""), 5, 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTokenizerUnterminatedStringTest,
    "PinWright.Model.Tokenizer.UnterminatedStringYieldsUnknownToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwTokenizerUnterminatedStringTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> Diagnostics;

    const bool bOk = FPwTokenizer::Tokenize(
        TEXTVIEW("materials {\n  Shell = \"/Game/Missing\n}\n"), Tokens, Diagnostics);

    TestFalse(TEXT("an unterminated string fails the lex"), bOk);
    TestEqual(TEXT("exactly one diagnostic - the scanner does not retry per character"), Diagnostics.Num(), 1);

    if (Diagnostics.Num() == 1)
    {
        const FPwDiagnostic& Diagnostic = Diagnostics[0];
        TestEqual(TEXT("diagnostic code"), Diagnostic.Code, FString(PwSourceDiagnosticCodes::PWSRC_UNTERMINATED_STRING));
        TestEqual(TEXT("diagnostic line"), Diagnostic.Line, 2);
        TestEqual(TEXT("diagnostic column"), Diagnostic.Column, 11);
        TestTrue(TEXT("diagnostic renders with code and position"),
            Diagnostic.ToString().StartsWith(TEXT("[PWSRC_UNTERMINATED_STRING] line 2, col 11: ")));
        // The offending fragment is escaped, so a diagnostic stays on one line.
        TestFalse(TEXT("rendered diagnostic carries no raw line break"), Diagnostic.ToString().Contains(TEXT("\n")));
    }

    // The run is emitted as one Unknown token and lexing continues: the closing brace
    // on the next line is still there for the parser to resynchronise on.
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 5, EPwTokenType::Unknown, TEXT("\"/Game/Missing"), 2, 11);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 6, EPwTokenType::Newline, TEXT(""), 2, 25);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 7, EPwTokenType::CloseBrace, TEXT("}"), 3, 1);

    TestEqual(TEXT("the stream still terminates in EndOfFile"),
        PwModelTokenizerTest_CountType(Tokens, EPwTokenType::EndOfFile), 1);

    // A closed literal on the same shape decodes to its value with the quotes stripped.
    TArray<FPwToken> GoodTokens;
    TArray<FPwDiagnostic> GoodDiagnostics;
    TestTrue(TEXT("closed literal lexes cleanly"),
        FPwTokenizer::Tokenize(TEXTVIEW("Shell = \"/Game/M_Metal\""), GoodTokens, GoodDiagnostics));
    PwModelTokenizerTest_ExpectToken(*this, GoodTokens, 2, EPwTokenType::String, TEXT("/Game/M_Metal"), 1, 9);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTokenizerPunctuationTest,
    "PinWright.Model.Tokenizer.TupleAndListPunctuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwTokenizerPunctuationTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> Diagnostics;

    const bool bOk = FPwTokenizer::Tokenize(
        TEXTVIEW("convex points=[(0, 0, 0), (1, 0, 0)]"), Tokens, Diagnostics);

    TestTrue(TEXT("tuple list lexes without diagnostics"), bOk);

    TestEqual(TEXT("one '['"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::OpenBracket), 1);
    TestEqual(TEXT("one ']'"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::CloseBracket), 1);
    TestEqual(TEXT("two '('"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::OpenParen), 2);
    TestEqual(TEXT("two ')'"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::CloseParen), 2);
    TestEqual(TEXT("five ','"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::Comma), 5);
    TestEqual(TEXT("one '='"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::Equals), 1);
    TestEqual(TEXT("six numbers"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::Number), 6);
    TestEqual(TEXT("no unknown tokens"), PwModelTokenizerTest_CountType(Tokens, EPwTokenType::Unknown), 0);

    PwModelTokenizerTest_ExpectToken(*this, Tokens, 3, EPwTokenType::OpenBracket, TEXT("["), 1, 15);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 4, EPwTokenType::OpenParen, TEXT("("), 1, 16);

    // Braces are punctuation too, and a nested block is just brace nesting to the lexer.
    TArray<FPwToken> BlockTokens;
    TArray<FPwDiagnostic> BlockDiagnostics;
    TestTrue(TEXT("nested block lexes without diagnostics"),
        FPwTokenizer::Tokenize(TEXTVIEW("subtract { sphere radius=18 }"), BlockTokens, BlockDiagnostics));
    TestEqual(TEXT("one '{'"), PwModelTokenizerTest_CountType(BlockTokens, EPwTokenType::OpenBrace), 1);
    TestEqual(TEXT("one '}'"), PwModelTokenizerTest_CountType(BlockTokens, EPwTokenType::CloseBrace), 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwTokenizerColumnTest,
    "PinWright.Model.Tokenizer.ColumnsTrackAcrossAndBetweenLines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwTokenizerColumnTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> Diagnostics;

    const bool bOk = FPwTokenizer::Tokenize(
        TEXTVIEW("  box size=(80, 50, 40)\n  bevel distance=2.5\n"), Tokens, Diagnostics);

    TestTrue(TEXT("lexes without diagnostics"), bOk);

    // Later tokens on a line carry their real column, not the column of the first
    // token or an offset from it.
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 0, EPwTokenType::Identifier, TEXT("box"), 1, 3);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 1, EPwTokenType::Identifier, TEXT("size"), 1, 7);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 2, EPwTokenType::Equals, TEXT("="), 1, 11);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 3, EPwTokenType::OpenParen, TEXT("("), 1, 12);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 4, EPwTokenType::Number, TEXT("80"), 1, 13);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 8, EPwTokenType::Number, TEXT("40"), 1, 21);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 9, EPwTokenType::CloseParen, TEXT(")"), 1, 23);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 10, EPwTokenType::Newline, TEXT(""), 1, 24);

    // Columns restart at 1 on the next line.
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 11, EPwTokenType::Identifier, TEXT("bevel"), 2, 3);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 12, EPwTokenType::Identifier, TEXT("distance"), 2, 9);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 13, EPwTokenType::Equals, TEXT("="), 2, 17);
    PwModelTokenizerTest_ExpectToken(*this, Tokens, 14, EPwTokenType::Number, TEXT("2.5"), 2, 18);

    // CRLF is one line break, not two: a \r\n file must not double every line number.
    TArray<FPwToken> CrlfTokens;
    TArray<FPwDiagnostic> CrlfDiagnostics;
    TestTrue(TEXT("CRLF source lexes without diagnostics"),
        FPwTokenizer::Tokenize(TEXTVIEW("pwmodel 0\r\npart body\r\n"), CrlfTokens, CrlfDiagnostics));
    TestEqual(TEXT("two newline tokens for two CRLF pairs"),
        PwModelTokenizerTest_CountType(CrlfTokens, EPwTokenType::Newline), 2);
    PwModelTokenizerTest_ExpectToken(*this, CrlfTokens, 3, EPwTokenType::Identifier, TEXT("part"), 2, 1);

    return true;
}
