// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwSource/PwParseCursor.h"

#include "PwSource/PwSuggest.h"

namespace
{
const FPwToken& PwParseCursor_EmptyToken()
{
    static const FPwToken Empty;
    return Empty;
}

FPwToken PwParseCursor_ValueToken(const FPwValue& Value)
{
    FPwToken Token;
    Token.Line = Value.Line;
    Token.Column = Value.Column;
    return Token;
}
}

FPwParseCursor::FPwParseCursor(const TArray<FPwToken>& InTokens, TArray<FPwDiagnostic>& InDiags)
    : Tokens(InTokens)
    , Diags(InDiags)
{
}

const FPwToken& FPwParseCursor::Peek(int32 Offset) const
{
    if (Tokens.Num() == 0)
    {
        return PwParseCursor_EmptyToken();
    }

    const int32 Index = FMath::Clamp(Pos + Offset, 0, Tokens.Num() - 1);
    return Tokens[Index];
}

bool FPwParseCursor::Check(EPwTokenType Type, int32 Offset) const
{
    return Peek(Offset).Type == Type;
}

bool FPwParseCursor::CheckIdentifier(const TCHAR* Text, int32 Offset) const
{
    return Check(EPwTokenType::Identifier, Offset) && Peek(Offset).Text == Text;
}

bool FPwParseCursor::AtEnd() const
{
    return Check(EPwTokenType::EndOfFile);
}

const FPwToken& FPwParseCursor::Advance()
{
    const FPwToken& Token = Peek();
    if (Tokens.Num() > 0 && Pos < Tokens.Num() - 1)
    {
        ++Pos;
    }
    return Token;
}

bool FPwParseCursor::Consume(EPwTokenType Type)
{
    if (!Check(Type))
    {
        return false;
    }

    Advance();
    return true;
}

void FPwParseCursor::SkipNewlines()
{
    while (Check(EPwTokenType::Newline))
    {
        Advance();
    }
}

void FPwParseCursor::SkipToNextLine()
{
    while (!AtEnd() && !Check(EPwTokenType::Newline))
    {
        Advance();
    }
    Consume(EPwTokenType::Newline);
}

void FPwParseCursor::Emit(EPwSeverity Severity, const TCHAR* Code, const FPwToken& At,
                          FString Message, TArray<FString> Suggestions)
{
    FPwDiagnostic Diagnostic;
    Diagnostic.Severity = Severity;
    Diagnostic.Line = At.Line;
    Diagnostic.Column = At.Column;
    Diagnostic.Code = Code;
    Diagnostic.Message = MoveTemp(Message);
    Diagnostic.ScopeLabel = ScopeLabel;
    Diagnostic.ScopeName = ScopeName;
    Diagnostic.Suggestions = MoveTemp(Suggestions);
    Diags.Add(MoveTemp(Diagnostic));
}

void FPwParseCursor::Error(const TCHAR* Code, const FPwToken& At, FString Message,
                           TArray<FString> Suggestions)
{
    Emit(EPwSeverity::Error, Code, At, MoveTemp(Message), MoveTemp(Suggestions));
}

void FPwParseCursor::Warn(const TCHAR* Code, const FPwToken& At, FString Message)
{
    Emit(EPwSeverity::Warning, Code, At, MoveTemp(Message));
}

void FPwParseCursor::ErrorAtLine(const TCHAR* Code, int32 Line, int32 Column, FString Message)
{
    FPwToken At;
    At.Line = Line;
    At.Column = Column;
    Error(Code, At, MoveTemp(Message));
}

bool FPwParseCursor::WasReportedByTokenizer() const
{
    return Check(EPwTokenType::Unknown);
}

void FPwParseCursor::UnexpectedToken(FString Expected)
{
    if (WasReportedByTokenizer())
    {
        return;
    }

    Error(PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN, Peek(),
        FString::Printf(TEXT("Expected %s but found %s."), *Expected,
            PwTokenTypeToString(Peek().Type)));
}

bool FPwParseCursor::ParseNumberInto(TArray<double>& Out)
{
    if (!Check(EPwTokenType::Number))
    {
        UnexpectedToken(TEXT("a number"));
        return false;
    }

    Out.Add(FCString::Atod(*Advance().Text));
    return true;
}

bool FPwParseCursor::ParseTupleBody(TArray<double>& Out)
{
    // The caller has consumed the opening parenthesis.
    if (Consume(EPwTokenType::CloseParen))
    {
        return true;
    }

    while (true)
    {
        if (Check(EPwTokenType::Newline) || AtEnd())
        {
            UnexpectedToken(TEXT("',' or ')' - a tuple may not span lines"));
            return false;
        }

        if (!ParseNumberInto(Out))
        {
            return false;
        }

        if (Consume(EPwTokenType::Comma))
        {
            continue;
        }

        if (Consume(EPwTokenType::CloseParen))
        {
            return true;
        }

        UnexpectedToken(TEXT("',' or ')'"));
        return false;
    }
}

bool FPwParseCursor::ParseValue(FPwValue& Out)
{
    const FPwToken Start = Peek();
    Out = FPwValue();
    Out.Line = Start.Line;
    Out.Column = Start.Column;

    switch (Start.Type)
    {
    case EPwTokenType::Number:
        Out.Type = EPwValueType::Number;
        Out.Number = FCString::Atod(*Advance().Text);
        return true;

    case EPwTokenType::String:
        Out.Type = EPwValueType::String;
        Out.Text = Advance().Text;
        return true;

    case EPwTokenType::Identifier:
        Out.Type = EPwValueType::Identifier;
        Out.Text = Advance().Text;
        return true;

    case EPwTokenType::OpenParen:
        Advance();
        Out.Type = EPwValueType::Tuple;
        return ParseTupleBody(Out.Tuple);

    case EPwTokenType::OpenBracket:
        Advance();
        Out.Type = EPwValueType::TupleList;
        if (Consume(EPwTokenType::CloseBracket))
        {
            return true;
        }

        while (true)
        {
            if (Check(EPwTokenType::Newline) || AtEnd())
            {
                UnexpectedToken(TEXT("',' or ']' - a list may not span lines"));
                return false;
            }

            if (!Consume(EPwTokenType::OpenParen))
            {
                UnexpectedToken(TEXT("'(' - a list holds tuples"));
                return false;
            }

            TArray<double> Tuple;
            if (!ParseTupleBody(Tuple))
            {
                return false;
            }
            Out.TupleList.Add(MoveTemp(Tuple));

            if (Consume(EPwTokenType::Comma))
            {
                continue;
            }

            if (Consume(EPwTokenType::CloseBracket))
            {
                return true;
            }

            UnexpectedToken(TEXT("',' or ']'"));
            return false;
        }

    default:
        UnexpectedToken(TEXT("a value"));
        return false;
    }
}

bool FPwParseCursor::ParseParams(TMap<FString, FPwValue>& OutParams, const FString& OwnerLabel)
{
    while (true)
    {
        if (Check(EPwTokenType::Newline) || Check(EPwTokenType::OpenBrace)
            || Check(EPwTokenType::CloseBrace) || AtEnd())
        {
            return true;
        }

        if (!Check(EPwTokenType::Identifier))
        {
            UnexpectedToken(FString::Printf(TEXT("a parameter for '%s'"), *OwnerLabel));
            SkipToNextLine();
            return false;
        }

        if (!Check(EPwTokenType::Equals, 1))
        {
            Error(PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN, Peek(),
                FString::Printf(TEXT("Expected '%s=' after '%s'. One statement per line: '%s' looks like a second statement on the same line."),
                    *Peek().Text, *OwnerLabel, *Peek().Text));
            SkipToNextLine();
            return false;
        }

        const FPwToken KeyToken = Advance();
        Advance(); // '='

        FPwValue Value;
        if (!ParseValue(Value))
        {
            SkipToNextLine();
            return false;
        }

        if (OutParams.Contains(KeyToken.Text))
        {
            Error(PwSourceDiagnosticCodes::PWSRC_DUPLICATE_PARAM, KeyToken,
                FString::Printf(TEXT("Parameter '%s' was given twice on '%s'."),
                    *KeyToken.Text, *OwnerLabel));
            continue;
        }

        OutParams.Add(KeyToken.Text, MoveTemp(Value));
    }
}

int32 FPwParseCursor::ExpectedTupleArity(EPwParamType Type)
{
    switch (Type)
    {
    case EPwParamType::Vector2:
    case EPwParamType::PointList2:
        return 2;
    case EPwParamType::Vector3:
    case EPwParamType::PointList3:
        return 3;
    case EPwParamType::Vector4:
    case EPwParamType::PointList4:
        return 4;
    case EPwParamType::FrameList:
        return 6;
    case EPwParamType::HarmonicList:
        return 3;
    default:
        return 0;
    }
}

void FPwParseCursor::BadValue(const FPwValue& Value, const FString& Owner,
                              const FPwParamSpec& Spec)
{
    Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, PwParseCursor_ValueToken(Value),
        FString::Printf(TEXT("Parameter '%s' on '%s' expects %s, but found %s."),
            *Spec.Name, *Owner, PwParamTypeToString(Spec.Type),
            PwValueTypeToString(Value.Type)));
}

void FPwParseCursor::ValidateRange(const FString& Owner, const FPwParamSpec& Spec,
                                   const FPwValue& Value)
{
    if (!Spec.bHasRange || Value.Type != EPwValueType::Number
        || (Value.Number >= Spec.MinValue && Value.Number <= Spec.MaxValue))
    {
        return;
    }

    Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, PwParseCursor_ValueToken(Value),
        FString::Printf(TEXT("Parameter '%s' on '%s' must be between %g and %g, but found %g."),
            *Spec.Name, *Owner, Spec.MinValue, Spec.MaxValue, Value.Number));
}

void FPwParseCursor::ValidateValue(const FString& Owner, const FPwParamSpec& Spec,
                                   const FPwValue& Value, const FPwParamPolicy& Policy)
{
    bool bHandled = false;
    if (Policy.OnValue)
    {
        bHandled = Policy.OnValue(Owner, Spec, Value) == EPwParamAction::Handled;
    }

    if (!bHandled)
    {
        const FPwToken At = PwParseCursor_ValueToken(Value);

        switch (Spec.Type)
        {
        case EPwParamType::Number:
            if (Value.Type != EPwValueType::Number)
            {
                BadValue(Value, Owner, Spec);
            }
            else
            {
                ValidateRange(Owner, Spec, Value);
            }
            break;

        case EPwParamType::Integer:
            if (Value.Type != EPwValueType::Number)
            {
                BadValue(Value, Owner, Spec);
            }
            else if (!FMath::IsNearlyEqual(Value.Number, FMath::RoundToDouble(Value.Number)))
            {
                Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                    FString::Printf(TEXT("Parameter '%s' on '%s' expects a whole number, but found %g."),
                        *Spec.Name, *Owner, Value.Number));
            }
            else
            {
                ValidateRange(Owner, Spec, Value);
            }
            break;

        case EPwParamType::Bool:
            if (Value.Type != EPwValueType::Identifier
                || (Value.Text != TEXT("true") && Value.Text != TEXT("false")))
            {
                Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                    FString::Printf(TEXT("Parameter '%s' on '%s' expects true or false."),
                        *Spec.Name, *Owner));
            }
            break;

        case EPwParamType::String:
            if (Value.Type != EPwValueType::String)
            {
                Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                    FString::Printf(TEXT("Parameter '%s' on '%s' expects a quoted string."),
                        *Spec.Name, *Owner));
            }
            break;

        case EPwParamType::Enum:
            if (Value.Type != EPwValueType::Identifier)
            {
                Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                    FString::Printf(TEXT("Parameter '%s' on '%s' expects one of: %s."),
                        *Spec.Name, *Owner, *FString::Join(Spec.AllowedValues, TEXT(", "))));
            }
            else if (!Spec.AllowedValues.Contains(Value.Text))
            {
                TArray<FString> Suggestions;
                const FString Guess = PwSuggest::Closest(Value.Text, Spec.AllowedValues);
                if (!Guess.IsEmpty())
                {
                    Suggestions.Add(Guess);
                }

                Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                    FString::Printf(TEXT("Unknown value '%s' for '%s' on '%s'. Valid values: %s."),
                        *Value.Text, *Spec.Name, *Owner,
                        *FString::Join(Spec.AllowedValues, TEXT(", "))),
                    MoveTemp(Suggestions));
            }
            break;

        case EPwParamType::Vector2:
        case EPwParamType::Vector3:
        case EPwParamType::Vector4:
        {
            if (Value.Type != EPwValueType::Tuple)
            {
                BadValue(Value, Owner, Spec);
                break;
            }

            const int32 Arity = ExpectedTupleArity(Spec.Type);
            if (Value.Tuple.Num() != Arity)
            {
                Error(PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY, At,
                    FString::Printf(TEXT("Parameter '%s' on '%s' expects a %d-component tuple, but found %d."),
                        *Spec.Name, *Owner, Arity, Value.Tuple.Num()));
            }
            break;
        }

        case EPwParamType::NumberList:
            if (Value.Type != EPwValueType::Tuple)
            {
                BadValue(Value, Owner, Spec);
            }
            break;

        case EPwParamType::PointList2:
        case EPwParamType::PointList3:
        case EPwParamType::PointList4:
        case EPwParamType::FrameList:
        case EPwParamType::HarmonicList:
        {
            if (Value.Type != EPwValueType::TupleList)
            {
                BadValue(Value, Owner, Spec);
                break;
            }

            const int32 Arity = ExpectedTupleArity(Spec.Type);
            for (int32 Index = 0; Index < Value.TupleList.Num(); ++Index)
            {
                if (Value.TupleList[Index].Num() != Arity)
                {
                    Error(PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY, At,
                        FString::Printf(TEXT("Entry %d of '%s' on '%s' expects %d components%s, but found %d."),
                            Index, *Spec.Name, *Owner, Arity, PwParamTypeTupleShape(Spec.Type),
                            Value.TupleList[Index].Num()));
                    break;
                }
            }
            break;
        }

        default:
            break;
        }
    }

    if (Policy.OnValueChecked)
    {
        Policy.OnValueChecked(Owner, Spec, Value);
    }
}

void FPwParseCursor::ValidateParams(const FPwToken& MissingAt, const FString& Owner,
                                    TArrayView<const FPwParamSpec> Specs,
                                    const TMap<FString, FPwValue>& Params,
                                    const FPwParamPolicy& Policy)
{
    for (const TPair<FString, FPwValue>& Pair : Params)
    {
        if (Policy.OnParam
            && Policy.OnParam(Owner, Pair.Key, Pair.Value) == EPwParamAction::Handled)
        {
            continue;
        }

        const FPwParamSpec* Spec = Specs.FindByPredicate(
            [&Pair](const FPwParamSpec& Candidate) { return Candidate.Name == Pair.Key; });
        if (!Spec)
        {
            TArray<FString> ValidNames;
            ValidNames.Reserve(Specs.Num());
            for (const FPwParamSpec& Candidate : Specs)
            {
                ValidNames.Add(Candidate.Name);
            }

            TArray<FString> Suggestions;
            const FString Guess = PwSuggest::Closest(Pair.Key, ValidNames);
            if (!Guess.IsEmpty())
            {
                Suggestions.Add(Guess);
            }

            const FString Hint = Policy.UnknownHint
                ? FString::Printf(TEXT(" %s"), Policy.UnknownHint)
                : FString();
            Error(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM, PwParseCursor_ValueToken(Pair.Value),
                FString::Printf(TEXT("'%s' has no parameter '%s'. Valid parameters: %s.%s"),
                    *Owner, *Pair.Key,
                    ValidNames.Num() > 0 ? *FString::Join(ValidNames, TEXT(", ")) : TEXT("(none)"),
                    *Hint),
                MoveTemp(Suggestions));
            continue;
        }

        ValidateValue(Owner, *Spec, Pair.Value, Policy);
    }

    for (const FPwParamSpec& Spec : Specs)
    {
        if (Spec.bRequired && !Params.Contains(Spec.Name))
        {
            Error(PwSourceDiagnosticCodes::PWSRC_MISSING_PARAM, MissingAt,
                FString::Printf(TEXT("'%s' requires parameter '%s' (%s)."),
                    *Owner, *Spec.Name, PwParamTypeToString(Spec.Type)));
        }
    }
}

FString FPwParseCursor::MissingBlockMessage(const TCHAR* Construct) const
{
    int32 Scan = Pos;
    while (Scan < Tokens.Num() && Tokens[Scan].Type == EPwTokenType::Newline)
    {
        ++Scan;
    }

    if (Scan > Pos && Scan < Tokens.Num() && Tokens[Scan].Type == EPwTokenType::OpenBrace)
    {
        return FString::Printf(
            TEXT("'%s' opens its '{ … }' block on the same line as the header. The '{' on line %d "
                 "begins a new statement, because a line break ends a statement in this format. "
                 "Move that '{' up to the end of the header line."),
            Construct, Tokens[Scan].Line);
    }

    return FString::Printf(
        TEXT("'%s' requires a '{ … }' block; write '{ }' for a leaf."), Construct);
}

void FPwParseCursor::ForEachBlockEntry(const FPwToken& OpenBrace, TFunctionRef<void()> Body)
{
    while (true)
    {
        SkipNewlines();

        if (Consume(EPwTokenType::CloseBrace))
        {
            return;
        }

        if (AtEnd())
        {
            ErrorAtLine(PwSourceDiagnosticCodes::PWSRC_UNCLOSED_BRACE, OpenBrace.Line,
                OpenBrace.Column, TEXT("This '{' is never closed."));
            return;
        }

        const int32 Before = Pos;
        Body();
        if (Pos == Before)
        {
            if (AtEnd())
            {
                ErrorAtLine(PwSourceDiagnosticCodes::PWSRC_UNCLOSED_BRACE, OpenBrace.Line,
                    OpenBrace.Column, TEXT("This '{' is never closed."));
                return;
            }
            Advance();
        }
    }
}

void FPwParseCursor::SkipBalancedBlock(const FPwToken& OpenBrace)
{
    int32 Depth = 1;
    while (Depth > 0)
    {
        if (AtEnd())
        {
            ErrorAtLine(PwSourceDiagnosticCodes::PWSRC_UNCLOSED_BRACE, OpenBrace.Line,
                OpenBrace.Column, TEXT("This '{' is never closed."));
            return;
        }

        if (Check(EPwTokenType::OpenBrace))
        {
            ++Depth;
        }
        else if (Check(EPwTokenType::CloseBrace))
        {
            --Depth;
        }
        Advance();
    }
}

void FPwParseCursor::SkipUnknownConstruct()
{
    while (!AtEnd() && !Check(EPwTokenType::Newline))
    {
        if (Check(EPwTokenType::OpenBrace))
        {
            const FPwToken BraceToken = Advance();
            SkipBalancedBlock(BraceToken);
            return;
        }
        Advance();
    }
    Consume(EPwTokenType::Newline);
}

bool FPwParseCursor::ParseOpStatement(FPwOp& OutOp, bool bIsFirstInList,
                                      IPwStatementSink& Sink)
{
    const FPwToken NameToken = Advance();
    OutOp = FPwOp();
    OutOp.OpName = NameToken.Text;
    OutOp.Line = NameToken.Line;
    OutOp.Column = NameToken.Column;

    if (!ParseParams(OutOp.Params, OutOp.OpName))
    {
        return false;
    }

    bool bHadBlock = false;
    if (Check(EPwTokenType::OpenBrace))
    {
        const FPwToken BraceToken = Advance();
        bHadBlock = true;
        IPwStatementSink& Nested = Sink.NestedSink();
        ParseOpList(OutOp.Children, BraceToken, Nested);
    }

    Sink.OnStatement(OutOp, bIsFirstInList, bHadBlock);
    return true;
}

void FPwParseCursor::ParseOpList(TArray<FPwOp>& OutOps, const FPwToken& OpenBrace,
                                 IPwStatementSink& Sink)
{
    ForEachBlockEntry(OpenBrace, [this, &OutOps, &Sink]()
    {
        if (!Check(EPwTokenType::Identifier))
        {
            UnexpectedToken(TEXT("an op name"));
            SkipToNextLine();
            return;
        }

        FPwOp Op;
        if (ParseOpStatement(Op, OutOps.Num() == 0, Sink))
        {
            OutOps.Add(MoveTemp(Op));
        }
    });
}

bool FPwParseCursor::ParseVersionHeader(const TCHAR* Keyword, const TCHAR* AcceptedVersions,
                                        TFunctionRef<bool(int32)> IsAccepted,
                                        FPwDocumentHeader& OutHeader)
{
    OutHeader = FPwDocumentHeader();
    SkipNewlines();

    if (!CheckIdentifier(Keyword))
    {
        Error(PwSourceDiagnosticCodes::PWSRC_MISSING_VERSION, Peek(),
            FString::Printf(TEXT("A source file must begin with a version header. Expected '%s 0' as the first non-comment, non-blank line."),
                Keyword));
        return false;
    }

    const FPwToken KeywordToken = Advance();
    OutHeader.FormatKeyword = KeywordToken.Text;
    OutHeader.VersionLine = KeywordToken.Line;
    OutHeader.VersionColumn = KeywordToken.Column;

    if (!Check(EPwTokenType::Number))
    {
        Error(PwSourceDiagnosticCodes::PWSRC_MISSING_VERSION, Peek(),
            FString::Printf(TEXT("The '%s' version header needs a version number. This build accepts: %s."),
                Keyword, AcceptedVersions));
        SkipToNextLine();
        return false;
    }

    const FPwToken NumberToken = Advance();
    const double Raw = FCString::Atod(*NumberToken.Text);
    const int32 Version = FMath::RoundToInt(Raw);
    if (!FMath::IsNearlyEqual(Raw, static_cast<double>(Version)) || !IsAccepted(Version))
    {
        Error(PwSourceDiagnosticCodes::PWSRC_UNSUPPORTED_VERSION, NumberToken,
            FString::Printf(TEXT("Unsupported document version '%s'. This build accepts: %s."),
                *NumberToken.Text, AcceptedVersions));
        return false;
    }

    OutHeader.Version = Version;

    if (!Check(EPwTokenType::Newline) && !AtEnd())
    {
        UnexpectedToken(TEXT("end of line after the version header"));
        SkipToNextLine();
        return false;
    }

    return true;
}

bool FPwParseCursor::ParseUse(TArrayView<const FString> ValidKinds, FPwUse& OutUse)
{
    OutUse = FPwUse();
    const FPwToken Keyword = Advance();
    OutUse.Line = Keyword.Line;
    OutUse.Column = Keyword.Column;

    if (!Check(EPwTokenType::Identifier))
    {
        UnexpectedToken(FString::Printf(TEXT("a kind: %s"), *FString::Join(ValidKinds, TEXT(", "))));
        SkipToNextLine();
        return false;
    }

    const FPwToken KindToken = Advance();
    OutUse.Kind = KindToken.Text;
    if (!ValidKinds.Contains(OutUse.Kind))
    {
        TArray<FString> Suggestions;
        const FString Guess = PwSuggest::Closest(OutUse.Kind, ValidKinds);
        if (!Guess.IsEmpty())
        {
            Suggestions.Add(Guess);
        }
        Error(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, KindToken,
            FString::Printf(TEXT("'use %s' is not a known kind. Valid kinds: %s."),
                *OutUse.Kind, *FString::Join(ValidKinds, TEXT(", "))),
            MoveTemp(Suggestions));
    }

    if (!CheckIdentifier(TEXT("from")))
    {
        UnexpectedToken(TEXT("'from'"));
        SkipToNextLine();
        return false;
    }
    Advance();

    if (!Check(EPwTokenType::String))
    {
        UnexpectedToken(TEXT("a quoted path"));
        SkipToNextLine();
        return false;
    }

    // Keep the decoded token text exactly as the tokenizer supplied it. The source core does
    // not interpret whether this is a relative source path or a /Game asset path.
    OutUse.Path = Advance().Text;
    return true;
}
