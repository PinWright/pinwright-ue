// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "PwSource/PwDiagnostic.h"
#include "PwSource/PwDocument.h"
#include "PwSource/PwParamSpec.h"
#include "PwSource/PwParseCursor.h"
#include "PwSource/PwToken.h"
#include "PwSource/PwTokenizer.h"
#include "PwSource/PwValue.h"

namespace PwSourceContractTestPrivate
{
    FPwToken MakeToken(EPwTokenType Type, const TCHAR* Text, int32 Line, int32 Column)
    {
        FPwToken Token;
        Token.Type = Type;
        Token.Text = Text;
        Token.Line = Line;
        Token.Column = Column;
        return Token;
    }

    void AddEndOfFile(TArray<FPwToken>& Tokens, int32 Line = 1, int32 Column = 1)
    {
        Tokens.Add(MakeToken(EPwTokenType::EndOfFile, TEXT(""), Line, Column));
    }

    const FPwToken* FindToken(const TArray<FPwToken>& Tokens, EPwTokenType Type,
                              const TCHAR* Text)
    {
        for (const FPwToken& Token : Tokens)
        {
            if (Token.Type == Type && Token.Text == Text)
            {
                return &Token;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceContractHeaderKeywordTest,
    "PinWright.core.pwsource_contract.HeaderCarriesTheCallerFormatKeyword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceContractHeaderKeywordTest::RunTest(const FString& Parameters)
{
    // This is deliberately not either format that currently exists. A hardcoded
    // {pwmodel,pwanim} implementation would pass the usual fixtures and still make
    // the shared cursor unusable by the next format.
    TArray<FPwToken> Tokens;
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::Identifier, TEXT("pwphysics"), 1, 1));
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::Number, TEXT("0"), 1, 11));
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::Newline, TEXT(""), 1, 12));
    PwSourceContractTestPrivate::AddEndOfFile(Tokens, 2, 1);

    TArray<FPwDiagnostic> Diagnostics;
    FPwParseCursor Cursor(Tokens, Diagnostics);
    FPwDocumentHeader Header;

    const bool bParsed = Cursor.ParseVersionHeader(
        TEXT("pwphysics"), TEXT("0"),
        [](int32 Version) { return Version == 0; }, Header);

    TestTrue(TEXT("a caller-owned format keyword parses"), bParsed);
    TestEqual(TEXT("the header keeps the exact caller keyword"),
        Header.FormatKeyword, FString(TEXT("pwphysics")));
    TestEqual(TEXT("the shared header still records the version"), Header.Version, 0);
    TestEqual(TEXT("the version line is retained"), Header.VersionLine, 1);
    TestEqual(TEXT("the clean header has no diagnostics"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceContractUsePathTest,
    "PinWright.core.pwsource_contract.UsePreservesItsExactPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceContractUsePathTest::RunTest(const FString& Parameters)
{
    TArray<FPwToken> Tokens;
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::Identifier, TEXT("use"), 7, 1));
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::Identifier, TEXT("future_kind"), 7, 5));
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::Identifier, TEXT("from"), 7, 14));
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::String, TEXT("/Game/Rigs/../Hero.Hero"), 7, 19));
    Tokens.Add(PwSourceContractTestPrivate::MakeToken(
        EPwTokenType::Newline, TEXT(""), 7, 43));
    PwSourceContractTestPrivate::AddEndOfFile(Tokens, 8, 1);

    TArray<FString> ValidKinds;
    ValidKinds.Add(TEXT("future_kind"));

    TArray<FPwDiagnostic> Diagnostics;
    FPwParseCursor Cursor(Tokens, Diagnostics);
    FPwUse Use;

    const bool bParsed = Cursor.ParseUse(TArrayView<const FString>(ValidKinds), Use);

    TestTrue(TEXT("a caller-owned kind list accepts its own kind"), bParsed);
    TestEqual(TEXT("the caller-owned use kind is preserved"),
        Use.Kind, FString(TEXT("future_kind")));
    TestEqual(TEXT("the use path is byte-exact, including /Game and .."),
        Use.Path, FString(TEXT("/Game/Rigs/../Hero.Hero")));
    TestEqual(TEXT("the use line is retained"), Use.Line, 7);
    TestEqual(TEXT("the valid use has no diagnostics"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceContractTokenVocabularyTest,
    "PinWright.core.pwsource_contract.TokenVocabularyIsFormatNeutral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceContractTokenVocabularyTest::RunTest(const FString& Parameters)
{
    // The tokenizer sees names, not formats. In particular, a new format keyword
    // must remain an ordinary identifier until that format's parser owns it.
    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bTokenized = FPwTokenizer::Tokenize(
        TEXT("pwmodel pwanim pwphysics bone\n"), Tokens, Diagnostics);

    TestTrue(TEXT("format-neutral words tokenize successfully"), bTokenized);
    TestTrue(TEXT("pwmodel remains an identifier in the shared tokenizer"),
        PwSourceContractTestPrivate::FindToken(
            Tokens, EPwTokenType::Identifier, TEXT("pwmodel")) != nullptr);
    TestTrue(TEXT("pwanim remains an identifier in the shared tokenizer"),
        PwSourceContractTestPrivate::FindToken(
            Tokens, EPwTokenType::Identifier, TEXT("pwanim")) != nullptr);
    TestTrue(TEXT("an unowned future format remains an identifier"),
        PwSourceContractTestPrivate::FindToken(
            Tokens, EPwTokenType::Identifier, TEXT("pwphysics")) != nullptr);
    TestTrue(TEXT("a format-neutral scope name remains an identifier"),
        PwSourceContractTestPrivate::FindToken(
            Tokens, EPwTokenType::Identifier, TEXT("bone")) != nullptr);
    TestEqual(TEXT("the neutral vocabulary emits one EOF token"),
        Tokens.Num() > 0 && Tokens.Last().Type == EPwTokenType::EndOfFile, true);
    TestEqual(TEXT("format-neutral words produce no lexical diagnostics"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceContractValueOperationShapeTest,
    "PinWright.core.pwsource_contract.ValueAndOperationShapesAreShared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceContractValueOperationShapeTest::RunTest(const FString& Parameters)
{
    FPwValue FrameList;
    FrameList.Type = EPwValueType::TupleList;
    TArray<double> FirstFrame;
    FirstFrame.Add(0.0);
    FirstFrame.Add(0.0);
    FirstFrame.Add(0.0);
    FirstFrame.Add(0.0);
    FirstFrame.Add(0.0);
    FirstFrame.Add(0.0);
    FrameList.TupleList.Add(MoveTemp(FirstFrame));

    TArray<double> SecondFrame;
    SecondFrame.Add(10.0);
    SecondFrame.Add(0.0);
    SecondFrame.Add(0.0);
    SecondFrame.Add(0.0);
    SecondFrame.Add(0.0);
    SecondFrame.Add(90.0);
    FrameList.TupleList.Add(MoveTemp(SecondFrame));
    FrameList.Line = 12;
    FrameList.Column = 9;

    FPwOp Parent;
    Parent.OpName = TEXT("bone");
    Parent.Line = 12;
    Parent.Column = 1;
    Parent.Params.Add(TEXT("frames"), FrameList);

    FPwOp Child;
    Child.OpName = TEXT("keys");
    FPwValue Channel;
    Channel.Type = EPwValueType::Identifier;
    Channel.Text = TEXT("rotation");
    Channel.Line = 13;
    Channel.Column = 5;
    Child.Params.Add(TEXT("channel"), MoveTemp(Channel));
    Parent.Children.Add(Child);

    TestEqual(TEXT("a shared operation keeps its name"), Parent.OpName, FString(TEXT("bone")));
    TestTrue(TEXT("a shared operation carries a parameter map"), Parent.Params.Contains(TEXT("frames")));
    const FPwValue* SharedFrame = Parent.Params.Find(TEXT("frames"));
    if (TestNotNull(TEXT("the shared operation stores the frame value"), SharedFrame))
    {
        TestEqual(TEXT("the shared value keeps its tuple-list type"),
            SharedFrame->Type, EPwValueType::TupleList);
        TestEqual(TEXT("a frame-list value keeps both entries"),
            SharedFrame->TupleList.Num(), 2);
        TestEqual(TEXT("source position belongs to the shared value"),
            SharedFrame->Line, 12);
    }
    TestEqual(TEXT("a shared operation keeps nested children"), Parent.Children.Num(), 1);
    if (Parent.Children.Num() == 1)
    {
        TestEqual(TEXT("the nested operation name is retained"),
            Parent.Children[0].OpName, FString(TEXT("keys")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceContractAnimationParamShapeTest,
    "PinWright.core.pwsource_contract.AnimationParameterShapesAreNamed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceContractAnimationParamShapeTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("frame-list has a named wire type"),
        FString(PwParamTypeToString(EPwParamType::FrameList)), FString(TEXT("frame_list")));
    TestEqual(TEXT("frame-list exposes its six named components"),
        FString(PwParamTypeTupleShape(EPwParamType::FrameList)),
        FString(TEXT(" (x, y, z, roll, pitch, yaw)")));
    TestEqual(TEXT("harmonic-list has a named wire type"),
        FString(PwParamTypeToString(EPwParamType::HarmonicList)), FString(TEXT("harmonic_list")));
    TestEqual(TEXT("harmonic-list exposes its three named components"),
        FString(PwParamTypeTupleShape(EPwParamType::HarmonicList)),
        FString(TEXT(" (order, amplitude, phase)")));
    TestEqual(TEXT("ordinary vector types do not borrow animation names"),
        FString(PwParamTypeTupleShape(EPwParamType::Vector3)), FString());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceContractDiagnosticNamespaceTest,
    "PinWright.core.pwsource_contract.SourceDiagnosticsUseTheSharedNamespace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceContractDiagnosticNamespaceTest::RunTest(const FString& Parameters)
{
    const TCHAR* const Codes[] =
    {
        PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_CHARACTER,
        PwSourceDiagnosticCodes::PWSRC_UNTERMINATED_STRING,
        PwSourceDiagnosticCodes::PWSRC_INVALID_STRING,
        PwSourceDiagnosticCodes::PWSRC_INVALID_NUMBER,
        PwSourceDiagnosticCodes::PWSRC_MISSING_VERSION,
        PwSourceDiagnosticCodes::PWSRC_UNSUPPORTED_VERSION,
        PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN,
        PwSourceDiagnosticCodes::PWSRC_UNCLOSED_BRACE,
        PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP,
        PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM,
        PwSourceDiagnosticCodes::PWSRC_DUPLICATE_PARAM,
        PwSourceDiagnosticCodes::PWSRC_MISSING_PARAM,
        PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY,
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE,
        PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK,
        PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE,
    };

    TSet<FString> UniqueCodes;
    for (const TCHAR* Code : Codes)
    {
        const FString CodeString(Code);
        TestTrue(*FString::Printf(TEXT("shared code '%s' uses the PWSRC namespace"), *CodeString),
            CodeString.StartsWith(TEXT("PWSRC_")));
        TestFalse(*FString::Printf(TEXT("shared code '%s' is not a model code"), *CodeString),
            CodeString.StartsWith(TEXT("PWMODEL_")));
        TestTrue(*FString::Printf(TEXT("shared code '%s' is non-empty"), *CodeString),
            !CodeString.IsEmpty());
        UniqueCodes.Add(CodeString);
    }

    TestEqual(TEXT("the shared diagnostic vocabulary has all sixteen distinct codes"),
        UniqueCodes.Num(), static_cast<int32>(UE_ARRAY_COUNT(Codes)));

    const FPwDiagnostic Diagnostic = FPwDiagnostic::MakeError(
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, 4, 8, TEXT("bad source value"));
    TestEqual(TEXT("a source diagnostic stores the shared code"),
        Diagnostic.Code, FString(TEXT("PWSRC_BAD_VALUE")));
    TestTrue(TEXT("the formatted source diagnostic exposes its shared code"),
        Diagnostic.ToString().Contains(TEXT("[PWSRC_BAD_VALUE]")));
    return true;
}
