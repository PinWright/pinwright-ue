// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwSource/PwTokenizer.h"

#include "IrCore/IrTextUtils.h"

namespace
{
bool IsPwIdentifierStart(TCHAR Ch)
{
    return (Ch >= TEXT('A') && Ch <= TEXT('Z'))
        || (Ch >= TEXT('a') && Ch <= TEXT('z'))
        || Ch == TEXT('_');
}

bool IsPwDigit(TCHAR Ch)
{
    return Ch >= TEXT('0') && Ch <= TEXT('9');
}

bool IsPwIdentifierChar(TCHAR Ch)
{
    return IsPwIdentifierStart(Ch) || IsPwDigit(Ch);
}

bool IsPwLineBreak(TCHAR Ch)
{
    return Ch == TEXT('\n') || Ch == TEXT('\r');
}

EPwTokenType PunctuationType(TCHAR Ch)
{
    switch (Ch)
    {
    case TEXT('='): return EPwTokenType::Equals;
    case TEXT(','): return EPwTokenType::Comma;
    case TEXT('{'): return EPwTokenType::OpenBrace;
    case TEXT('}'): return EPwTokenType::CloseBrace;
    case TEXT('('): return EPwTokenType::OpenParen;
    case TEXT(')'): return EPwTokenType::CloseParen;
    case TEXT('['): return EPwTokenType::OpenBracket;
    case TEXT(']'): return EPwTokenType::CloseBracket;
    default:        return EPwTokenType::Unknown;
    }
}
}

bool FPwTokenizer::Tokenize(FStringView Source, TArray<FPwToken>& OutTokens,
                            TArray<FPwDiagnostic>& OutDiagnostics)
{
    OutTokens.Reset();
    OutDiagnostics.Reset();

    const int32 Len = Source.Len();
    int32 Index = 0;
    int32 Line = 1;
    int32 LineStart = 0;
    bool bHadError = false;

    const auto ColumnAt = [&LineStart](int32 At)
    {
        return At - LineStart + 1;
    };

    const auto Emit = [&OutTokens](EPwTokenType Type, FString Text, int32 TokenLine, int32 TokenColumn)
    {
        FPwToken Token;
        Token.Type = Type;
        Token.Text = MoveTemp(Text);
        Token.Line = TokenLine;
        Token.Column = TokenColumn;
        OutTokens.Add(MoveTemp(Token));
    };

    const auto Slice = [&Source](int32 Start, int32 End)
    {
        return FString(Source.Mid(Start, End - Start));
    };

    while (Index < Len)
    {
        const TCHAR Ch = Source[Index];
        const int32 Column = ColumnAt(Index);

        if (Ch == TEXT(' ') || Ch == TEXT('\t'))
        {
            ++Index;
            continue;
        }

        // Comments disappear, but their terminating newline remains a token so a
        // comment-only line cannot join the statements on either side of it.
        if (Ch == TEXT('#'))
        {
            while (Index < Len && !IsPwLineBreak(Source[Index]))
            {
                ++Index;
            }
            continue;
        }

        if (IsPwLineBreak(Ch))
        {
            Emit(EPwTokenType::Newline, FString(), Line, Column);

            if (Ch == TEXT('\r') && Index + 1 < Len && Source[Index + 1] == TEXT('\n'))
            {
                ++Index;
            }
            ++Index;
            ++Line;
            LineStart = Index;
            continue;
        }

        if (IsPwIdentifierStart(Ch))
        {
            int32 Scan = Index;
            while (Scan < Len && IsPwIdentifierChar(Source[Scan]))
            {
                ++Scan;
            }
            Emit(EPwTokenType::Identifier, Slice(Index, Scan), Line, Column);
            Index = Scan;
            continue;
        }

        // A leading '-' belongs to a number only when a digit follows it.
        const bool bStartsNumber = IsPwDigit(Ch)
            || (Ch == TEXT('-') && Index + 1 < Len && IsPwDigit(Source[Index + 1]));

        if (bStartsNumber)
        {
            int32 Scan = Index;
            if (Source[Scan] == TEXT('-'))
            {
                ++Scan;
            }
            while (Scan < Len && IsPwDigit(Source[Scan]))
            {
                ++Scan;
            }

            // Keep a trailing '.' as its own lexical error rather than silently
            // accepting it as a number with an absent fractional digit.
            if (Scan + 1 < Len && Source[Scan] == TEXT('.') && IsPwDigit(Source[Scan + 1]))
            {
                Scan += 2;
                while (Scan < Len && IsPwDigit(Source[Scan]))
                {
                    ++Scan;
                }
            }

            if (Scan < Len && (Source[Scan] == TEXT('e') || Source[Scan] == TEXT('E')))
            {
                int32 ExponentScan = Scan + 1;
                if (ExponentScan < Len
                    && (Source[ExponentScan] == TEXT('+') || Source[ExponentScan] == TEXT('-')))
                {
                    ++ExponentScan;
                }

                if (ExponentScan < Len && IsPwDigit(Source[ExponentScan]))
                {
                    while (ExponentScan < Len && IsPwDigit(Source[ExponentScan]))
                    {
                        ++ExponentScan;
                    }
                    Scan = ExponentScan;
                }
                else
                {
                    const FString Text = Slice(Index, ExponentScan);
                    OutDiagnostics.Add(FPwDiagnostic::MakeError(
                        PwSourceDiagnosticCodes::PWSRC_INVALID_NUMBER, Line, Column,
                        FString::Printf(
                            TEXT("Numeric literal '%s' has an exponent marker but no exponent digits."),
                            *Text)));
                    bHadError = true;

                    Emit(EPwTokenType::Unknown, Text, Line, Column);
                    Index = ExponentScan;
                    continue;
                }
            }

            Emit(EPwTokenType::Number, Slice(Index, Scan), Line, Column);
            Index = Scan;
            continue;
        }

        if (Ch == TEXT('"'))
        {
            int32 Scan = Index + 1;
            bool bClosed = false;

            while (Scan < Len)
            {
                const TCHAR Inner = Source[Scan];

                if (IsPwLineBreak(Inner))
                {
                    break;
                }

                if (Inner == TEXT('\\'))
                {
                    // An escape cannot consume a line break or the end of input.
                    if (Scan + 1 >= Len || IsPwLineBreak(Source[Scan + 1]))
                    {
                        break;
                    }
                    Scan += 2;
                    continue;
                }

                if (Inner == TEXT('"'))
                {
                    ++Scan;
                    bClosed = true;
                    break;
                }

                ++Scan;
            }

            const FString Raw = Slice(Index, Scan);
            if (!bClosed)
            {
                OutDiagnostics.Add(FPwDiagnostic::MakeError(
                    PwSourceDiagnosticCodes::PWSRC_UNTERMINATED_STRING, Line, Column,
                    FString::Printf(
                        TEXT("Unterminated string literal '%s'; strings may not span lines, use \\n."),
                        *FIrTextUtils::EscapeString(Raw))));
                bHadError = true;

                Emit(EPwTokenType::Unknown, Raw, Line, Column);
                Index = Scan;
                continue;
            }

            FString Value;
            FString DecodeError;
            if (!FIrTextUtils::TryUnwrapStringLiteral(Raw, Value, DecodeError))
            {
                OutDiagnostics.Add(FPwDiagnostic::MakeError(
                    PwSourceDiagnosticCodes::PWSRC_INVALID_STRING, Line, Column,
                    FString::Printf(
                        TEXT("Invalid string literal '%s': %s"),
                        *FIrTextUtils::EscapeString(Raw), *DecodeError)));
                bHadError = true;

                Emit(EPwTokenType::Unknown, Raw, Line, Column);
                Index = Scan;
                continue;
            }

            Emit(EPwTokenType::String, MoveTemp(Value), Line, Column);
            Index = Scan;
            continue;
        }

        const EPwTokenType Punctuation = PunctuationType(Ch);
        if (Punctuation != EPwTokenType::Unknown)
        {
            Emit(Punctuation, FString(Source.Mid(Index, 1)), Line, Column);
            ++Index;
            continue;
        }

        const FString Text = FString(Source.Mid(Index, 1));
        OutDiagnostics.Add(FPwDiagnostic::MakeError(
            PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_CHARACTER, Line, Column,
            FString::Printf(TEXT("Unexpected character '%s'."), *FIrTextUtils::EscapeString(Text))));
        bHadError = true;

        Emit(EPwTokenType::Unknown, Text, Line, Column);
        ++Index;
    }

    // Do not synthesize a trailing newline: EOF is enough to terminate the last
    // statement, and a final line break is a real input distinction.
    Emit(EPwTokenType::EndOfFile, FString(), Line, ColumnAt(Index));
    return !bHadError;
}
