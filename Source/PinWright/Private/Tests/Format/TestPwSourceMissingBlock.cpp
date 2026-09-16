// Copyright (c) 2026 Alexander Penkin. MIT License.

// A block's '{' must sit on the header line, because a line break ends a statement in these
// formats. That rule is fine; what was not fine is that the diagnostic for breaking it said
// "requires a '{ … }' block" while the author's block sat one line below, so the message named
// a remedy that was already applied. These tests pin the message apart from the genuinely
// blockless case, and pin the header diagnostics that the same early return used to discard.
#include "Misc/AutomationTest.h"

#include "PwAnim/PwAnimAst.h"
#include "PwAnim/PwAnimParser.h"
#include "PwSkel/PwSkelAst.h"
#include "PwSkel/PwSkelParser.h"
#include "PwSource/PwDiagnostic.h"

namespace
{
    const FPwDiagnostic* PwMissingBlockTest_Find(const TArray<FPwDiagnostic>& Diagnostics,
                                                 const TCHAR* Code)
    {
        for (const FPwDiagnostic& Diagnostic : Diagnostics)
        {
            if (Diagnostic.Code == Code)
            {
                return &Diagnostic;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelAllmanBraceNamesTheLineBreakTest,
    "PinWright.Format.MissingBlock.SkeletonBraceOnNextLineNamesTheLineBreak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelAllmanBraceNamesTheLineBreakTest::RunTest(const FString& Parameters)
{
    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwSkelParser::Parse(
        TEXTVIEW("pwskel 0\n")
        TEXT("bone \"root\"\n")
        TEXT("{\n")
        TEXT("}\n"),
        Document, Diagnostics);

    const FPwDiagnostic* BadBlock = PwMissingBlockTest_Find(
        Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK);
    TestNotNull(*FString::Printf(TEXT("PWSRC_BAD_BLOCK reported. [%s]"),
        *JoinPwDiagnostics(Diagnostics)), BadBlock);

    if (BadBlock)
    {
        // The remedy must be the one the author has not already applied. "requires a block" is
        // the wrong half of the message here and was what shipped.
        TestTrue(TEXT("message names the same-line rule"),
            BadBlock->Message.Contains(TEXT("same line as the header")));
        TestTrue(TEXT("message cites the orphaned brace's line"),
            BadBlock->Message.Contains(TEXT("line 3")));
        TestFalse(TEXT("message does not claim the block is absent"),
            BadBlock->Message.Contains(TEXT("requires a '{")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelGenuinelyBlocklessBoneStillSaysSoTest,
    "PinWright.Format.MissingBlock.SkeletonBonelessBoneStillReportsAbsentBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelGenuinelyBlocklessBoneStillSaysSoTest::RunTest(const FString& Parameters)
{
    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwSkelParser::Parse(
        TEXTVIEW("pwskel 0\n")
        TEXT("bone \"root\"\n"),
        Document, Diagnostics);

    const FPwDiagnostic* BadBlock = PwMissingBlockTest_Find(
        Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK);
    TestNotNull(*FString::Printf(TEXT("PWSRC_BAD_BLOCK reported. [%s]"),
        *JoinPwDiagnostics(Diagnostics)), BadBlock);

    if (BadBlock)
    {
        // No brace follows at all, so the original wording is the correct one and must survive.
        TestTrue(TEXT("message says the block is required"),
            BadBlock->Message.Contains(TEXT("requires a '{")));
        TestFalse(TEXT("message does not invent an orphaned brace"),
            BadBlock->Message.Contains(TEXT("same line as the header")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelMissingBlockKeepsHeaderDiagnosticsTest,
    "PinWright.Format.MissingBlock.SkeletonHeaderErrorsSurviveAMissingBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelMissingBlockKeepsHeaderDiagnosticsTest::RunTest(const FString& Parameters)
{
    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwSkelParser::Parse(
        TEXTVIEW("pwskel 0\n")
        TEXT("bone \"root\" at=(0, 0)\n")
        TEXT("{\n")
        TEXT("}\n"),
        Document, Diagnostics);

    // Both errors were visible to the parser in one pass. Reporting only the block error made a
    // two-error file take two round trips, and the second error appeared only once the first
    // was fixed - which reads as a new regression rather than a known one.
    TestNotNull(*FString::Printf(TEXT("the bad 'at' arity is still reported. [%s]"),
        *JoinPwDiagnostics(Diagnostics)),
        PwMissingBlockTest_Find(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY));
    TestNotNull(TEXT("the block error is reported too"),
        PwMissingBlockTest_Find(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimAllmanBraceNamesTheLineBreakTest,
    "PinWright.Format.MissingBlock.AnimationBraceOnNextLineNamesTheLineBreak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwAnimAllmanBraceNamesTheLineBreakTest::RunTest(const FString& Parameters)
{
    FPwAnimDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwAnimParser::Parse(
        TEXTVIEW("pwanim 0\n")
        TEXT("timebase rate=(30, 1) frames=30\n")
        TEXT("bone \"upper_arm\"\n")
        TEXT("{\n")
        TEXT("    key frame=0 rotate=(0, 0, 0)\n")
        TEXT("}\n"),
        Document, Diagnostics);

    const FPwDiagnostic* BadBlock = PwMissingBlockTest_Find(
        Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK);
    TestNotNull(*FString::Printf(TEXT("PWSRC_BAD_BLOCK reported. [%s]"),
        *JoinPwDiagnostics(Diagnostics)), BadBlock);

    if (BadBlock)
    {
        // .pwanim carried a byte-identical copy of the wrong message. The fix is shared, so the
        // two formats cannot drift apart again.
        TestTrue(TEXT("message names the same-line rule"),
            BadBlock->Message.Contains(TEXT("same line as the header")));
        TestTrue(TEXT("message cites the orphaned brace's line"),
            BadBlock->Message.Contains(TEXT("line 4")));
    }

    return true;
}
