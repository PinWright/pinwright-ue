// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "PwSource/PwParseCursor.h"

namespace
{
FPwToken PwSourceCursorTest_Token(EPwTokenType Type, const TCHAR* Text, int32 Line = 1,
                                  int32 Column = 1)
{
    FPwToken Token;
    Token.Type = Type;
    Token.Text = Text ? Text : TEXT("");
    Token.Line = Line;
    Token.Column = Column;
    return Token;
}

bool PwSourceCursorTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return true;
        }
    }
    return false;
}

struct FPwSourceCursorTestSink final : IPwStatementSink
{
    int32 StatementCount = 0;
    int32 BlockCount = 0;
    IPwStatementSink* Child = nullptr;

    virtual void OnStatement(const FPwOp&, bool, bool bHadBlock) override
    {
        ++StatementCount;
        BlockCount += bHadBlock ? 1 : 0;
    }

    virtual IPwStatementSink& NestedSink() override
    {
        return Child ? *Child : *this;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceCursorVersionHeaderTest,
    "PinWright.core.pwsource.VersionHeaderTakesItsKeywordFromTheCaller",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceCursorVersionHeaderTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens = {
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("customdoc"), 3, 1),
        PwSourceCursorTest_Token(EPwTokenType::Number, TEXT("0"), 3, 11),
        PwSourceCursorTest_Token(EPwTokenType::Newline, nullptr, 3, 12),
        PwSourceCursorTest_Token(EPwTokenType::EndOfFile, nullptr, 4, 1),
    };
    TArray<FPwDiagnostic> Diagnostics;
    FPwParseCursor Cursor(Tokens, Diagnostics);
    FPwDocumentHeader Header;

    const bool bParsed = Cursor.ParseVersionHeader(
        TEXT("customdoc"), TEXT("0"), [](int32 Version) { return Version == 0; }, Header);

    TestTrue(TEXT("the caller-owned keyword parses"), bParsed);
    TestEqual(TEXT("the header records the exact caller keyword"), Header.FormatKeyword,
        FString(TEXT("customdoc")));
    TestEqual(TEXT("the version is recorded"), Header.Version, 0);
    TestEqual(TEXT("a clean header emits no diagnostics"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceCursorUseKindTest,
    "PinWright.core.pwsource.UseKindListComesFromTheCaller",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceCursorUseKindTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens = {
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("use")),
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("skeleton"), 1, 5),
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("from"), 1, 14),
        PwSourceCursorTest_Token(EPwTokenType::String, TEXT("/Game/Rigs/Hero"), 1, 19),
        PwSourceCursorTest_Token(EPwTokenType::Newline, nullptr),
        PwSourceCursorTest_Token(EPwTokenType::EndOfFile, nullptr),
    };
    TArray<FString> ValidKinds = { TEXT("skeleton") };
    TArray<FPwDiagnostic> Diagnostics;
    FPwParseCursor Cursor(Tokens, Diagnostics);
    FPwUse Use;

    const bool bParsed = Cursor.ParseUse(
        TArrayView<const FString>(ValidKinds.GetData(), ValidKinds.Num()), Use);

    TestTrue(TEXT("the caller-owned kind parses"), bParsed);
    TestEqual(TEXT("the kind is retained"), Use.Kind, FString(TEXT("skeleton")));
    TestEqual(TEXT("the path is byte-exact, including /Game/"), Use.Path,
        FString(TEXT("/Game/Rigs/Hero")));
    TestEqual(TEXT("a valid use emits no diagnostics"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceCursorMalformedBlockTest,
    "PinWright.core.pwsource.CursorRefusesToSpinOnAMalformedBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceCursorMalformedBlockTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens = {
        PwSourceCursorTest_Token(EPwTokenType::OpenBrace, TEXT("{"), 7, 9),
        PwSourceCursorTest_Token(EPwTokenType::Unknown, TEXT("@"), 8, 5),
        PwSourceCursorTest_Token(EPwTokenType::Unknown, TEXT("@"), 8, 7),
        PwSourceCursorTest_Token(EPwTokenType::Unknown, TEXT("@"), 8, 9),
        PwSourceCursorTest_Token(EPwTokenType::EndOfFile, nullptr, 9, 1),
    };
    TArray<FPwDiagnostic> Diagnostics;
    FPwParseCursor Cursor(Tokens, Diagnostics);
    const FPwToken OpenBrace = Tokens[0];
    Cursor.Advance();

    Cursor.ForEachBlockEntry(OpenBrace, []() {});

    TestEqual(TEXT("the no-progress guard emits exactly one diagnostic"), Diagnostics.Num(), 1);
    if (Diagnostics.Num() == 1)
    {
        TestTrue(TEXT("the diagnostic is an unclosed brace"),
            Diagnostics[0].Code == PwSourceDiagnosticCodes::PWSRC_UNCLOSED_BRACE);
        TestEqual(TEXT("the diagnostic points at the opening line"), Diagnostics[0].Line, 7);
        TestEqual(TEXT("the diagnostic points at the opening column"), Diagnostics[0].Column, 9);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceCursorParamPolicyTest,
    "PinWright.core.pwsource.ParamPolicyCanSuppressTheGenericEnumError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceCursorParamPolicyTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens = {
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("mode")),
        PwSourceCursorTest_Token(EPwTokenType::Equals, TEXT("="), 1, 5),
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("spherical"), 1, 6),
        PwSourceCursorTest_Token(EPwTokenType::EndOfFile, nullptr),
    };
    TArray<FPwDiagnostic> Diagnostics;
    FPwParseCursor Cursor(Tokens, Diagnostics);
    TMap<FString, FPwValue> Params;
    Cursor.ParseParams(Params, TEXT("uv"));

    FPwParamSpec Mode;
    Mode.Name = TEXT("mode");
    Mode.Type = EPwParamType::Enum;
    Mode.AllowedValues = { TEXT("box"), TEXT("cylindrical") };
    const TArray<FPwParamSpec> Specs = { Mode };
    FPwToken MissingAt = PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("uv"));

    Cursor.ValidateParams(MissingAt, TEXT("uv"), Specs, Params);
    TestTrue(TEXT("the empty policy keeps the generic enum validation"),
        PwSourceCursorTest_HasCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));

    Diagnostics.Reset();
    bool bPolicyCalled = false;
    FPwParamPolicy Policy;
    Policy.OnValue = [&bPolicyCalled](const FString&, const FPwParamSpec&, const FPwValue&)
    {
        bPolicyCalled = true;
        return EPwParamAction::Handled;
    };
    Cursor.ValidateParams(MissingAt, TEXT("uv"), Specs, Params, Policy);

    TestTrue(TEXT("the format hook ran"), bPolicyCalled);
    TestEqual(TEXT("Handled suppresses the generic enum error"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceCursorNestedSinkTest,
    "PinWright.core.pwsource.NestedBlockRulesComeFromTheSink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceCursorNestedSinkTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens = {
        PwSourceCursorTest_Token(EPwTokenType::OpenBrace, TEXT("{")),
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("outer"), 1, 3),
        PwSourceCursorTest_Token(EPwTokenType::OpenBrace, TEXT("{"), 1, 9),
        PwSourceCursorTest_Token(EPwTokenType::Identifier, TEXT("inner"), 1, 11),
        PwSourceCursorTest_Token(EPwTokenType::CloseBrace, TEXT("}"), 1, 17),
        PwSourceCursorTest_Token(EPwTokenType::CloseBrace, TEXT("}"), 1, 19),
        PwSourceCursorTest_Token(EPwTokenType::EndOfFile, nullptr),
    };
    TArray<FPwDiagnostic> Diagnostics;
    FPwParseCursor Cursor(Tokens, Diagnostics);
    TArray<FPwOp> Ops;
    FPwSourceCursorTestSink Parent;
    FPwSourceCursorTestSink Child;
    Parent.Child = &Child;
    const FPwToken OpenBrace = Tokens[0];
    Cursor.Advance();

    Cursor.ParseOpList(Ops, OpenBrace, Parent);

    TestEqual(TEXT("the parent sink saw the outer statement"), Parent.StatementCount, 1);
    TestEqual(TEXT("the parent sink saw its block"), Parent.BlockCount, 1);
    TestEqual(TEXT("the distinct nested sink saw the inner statement"), Child.StatementCount, 1);
    TestEqual(TEXT("the nested sink has no unexpected diagnostics"), Diagnostics.Num(), 0);
    return true;
}
