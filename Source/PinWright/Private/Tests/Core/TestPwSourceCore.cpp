// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "PwSource/PwDiagnostic.h"
#include "PwSource/PwSuggest.h"
#include "PwSource/PwTokenizer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceDiagnosticScopeTest,
    "PinWright.Source.Core.DiagnosticScopeIsLabelledNotAssumed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSourceDiagnosticScopeTest::RunTest(const FString& Parameters)
{
    FPwDiagnostic Part = FPwDiagnostic::MakeError(
        TEXT("PWSRC_BAD_VALUE"), 4, 7, TEXT("bad value"));
    Part.ScopeLabel = TEXT("part");
    Part.ScopeName = TEXT("body");
    TestEqual(TEXT("part diagnostics keep their explicit scope label"), Part.ToString(),
        FString(TEXT("[PWSRC_BAD_VALUE] line 4, col 7: bad value (part \"body\")")));

    FPwDiagnostic Bone = FPwDiagnostic::MakeError(
        TEXT("PWSRC_BAD_VALUE"), 9, 3, TEXT("bad value"));
    Bone.ScopeLabel = TEXT("bone");
    Bone.ScopeName = TEXT("pelvis");
    TestEqual(TEXT("other formats are not rendered as parts"), Bone.ToString(),
        FString(TEXT("[PWSRC_BAD_VALUE] line 9, col 3: bad value (bone \"pelvis\")")));

    FPwDiagnostic Unlabelled = FPwDiagnostic::MakeError(
        TEXT("PWSRC_BAD_VALUE"), 1, 1, TEXT("bad value"));
    Unlabelled.ScopeName = TEXT("mystery");
    TestTrue(TEXT("an absent label is visible rather than silently dropped"),
        Unlabelled.ToString().Contains(TEXT(" (scope \"mystery\")")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceSuggestClosestTest,
    "PinWright.Source.Core.SuggestClosestFindsATransposition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSourceSuggestClosestTest::RunTest(const FString& Parameters)
{
    const TArray<FString> Candidates = { TEXT("box"), TEXT("sphere"), TEXT("capsule") };
    TestEqual(TEXT("Levenshtein tier catches a transposition"),
        PwSuggest::Closest(TEXT("spehre"), Candidates), FString(TEXT("sphere")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceTokenizerCoreTest,
    "PinWright.Source.Core.TokenizerIsTotalAndKeepsPositions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSourceTokenizerCoreTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> Diagnostics;
    TestTrue(TEXT("valid source tokenizes"),
        FPwTokenizer::Tokenize(TEXTVIEW("part body {\n"), Tokens, Diagnostics));
    TestEqual(TEXT("valid source has no diagnostics"), Diagnostics.Num(), 0);
    TestTrue(TEXT("token stream ends with EOF"), Tokens.Num() > 0
        && Tokens.Last().Type == EPwTokenType::EndOfFile);
    if (Tokens.Num() >= 2)
    {
        TestEqual(TEXT("the first identifier is on line one"), Tokens[0].Line, 1);
        TestEqual(TEXT("the second identifier preserves its column"), Tokens[1].Column, 6);
    }

    TArray<FPwToken> BadTokens;
    TArray<FPwDiagnostic> BadDiagnostics;
    TestFalse(TEXT("a bad character is reported"),
        FPwTokenizer::Tokenize(TEXTVIEW("@"), BadTokens, BadDiagnostics));
    TestEqual(TEXT("bad input still has one lexical diagnostic"), BadDiagnostics.Num(), 1);
    TestTrue(TEXT("bad input still has an EOF token"), BadTokens.Num() > 0
        && BadTokens.Last().Type == EPwTokenType::EndOfFile);
    return true;
}
