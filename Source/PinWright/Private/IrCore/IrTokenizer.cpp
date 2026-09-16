// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "IrCore/IrTokenizer.h"

#include "IrCore/IIrGrammar.h"

namespace
{
void ReadWord(const FString& Line, int32& Pos, const IIrGrammar& Grammar, TArray<FIrToken>& OutTokens)
{
    const int32 Start = Pos;

    while (Pos < Line.Len() && (FChar::IsAlnum(Line[Pos]) || Line[Pos] == TEXT('_')))
    {
        ++Pos;
    }

    const FString Word = Line.Mid(Start, Pos - Start);

    FIrToken Token;
    Token.Text = Word;
    Token.Position = Start;
    Token.Type = FIrTokenizer::IsKeyword(Word, Grammar) ? EIrTokenType::Keyword : EIrTokenType::Identifier;
    OutTokens.Add(MoveTemp(Token));
}

void ReadString(const FString& Line, int32& Pos, TArray<FIrToken>& OutTokens)
{
    const int32 Start = Pos;
    ++Pos;

    while (Pos < Line.Len())
    {
        if (Line[Pos] == TEXT('\\') && Pos + 1 < Line.Len())
        {
            Pos += 2;
            continue;
        }
        if (Line[Pos] == TEXT('"'))
        {
            ++Pos;
            break;
        }
        ++Pos;
    }

    FIrToken Token;
    Token.Type = EIrTokenType::StringLiteral;
    Token.Text = Line.Mid(Start, Pos - Start);
    Token.Position = Start;
    OutTokens.Add(MoveTemp(Token));
}

void ReadNumber(const FString& Line, int32& Pos, TArray<FIrToken>& OutTokens)
{
    const int32 Start = Pos;

    if (Pos < Line.Len() && Line[Pos] == TEXT('-'))
    {
        ++Pos;
    }

    while (Pos < Line.Len() && FChar::IsDigit(Line[Pos]))
    {
        ++Pos;
    }

    if (Pos < Line.Len() && Line[Pos] == TEXT('.') && Pos + 1 < Line.Len() && FChar::IsDigit(Line[Pos + 1]))
    {
        ++Pos;
        while (Pos < Line.Len() && FChar::IsDigit(Line[Pos]))
        {
            ++Pos;
        }
    }

    FIrToken Token;
    Token.Type = EIrTokenType::NumberLiteral;
    Token.Text = Line.Mid(Start, Pos - Start);
    Token.Position = Start;
    OutTokens.Add(MoveTemp(Token));
}

void ReadSigilToken(const FString& Line, int32& Pos, TArray<FIrToken>& OutTokens)
{
    const int32 Start = Pos;
    const TCHAR Sigil = Line[Pos];
    ++Pos;

    while (Pos < Line.Len() && (FChar::IsAlnum(Line[Pos]) || Line[Pos] == TEXT('_')))
    {
        ++Pos;
    }

    FIrToken Token;
    Token.Text = Line.Mid(Start, Pos - Start);
    Token.Position = Start;

    if (Sigil == TEXT('%'))
    {
        Token.Type = EIrTokenType::PercentRef;
    }
    else if (Sigil == TEXT('$'))
    {
        Token.Type = EIrTokenType::DollarRef;
    }
    else
    {
        if (Pos < Line.Len() && Line[Pos] == TEXT(':'))
        {
            ++Pos;
            Token.Type = EIrTokenType::LabelDef;
        }
        else
        {
            Token.Type = EIrTokenType::LabelRef;
        }
    }

    OutTokens.Add(MoveTemp(Token));
}
}

bool FIrTokenizer::IsKeyword(const FString& Word, const IIrGrammar& Grammar)
{
    return Grammar.IsKeyword(Word);
}

TArray<FIrToken> FIrTokenizer::Tokenize(const FString& Line, const IIrGrammar& Grammar)
{
    TArray<FIrToken> Tokens;
    int32 Pos = 0;

    while (Pos < Line.Len())
    {
        const TCHAR Ch = Line[Pos];

        if (FChar::IsWhitespace(Ch))
        {
            ++Pos;
            continue;
        }

        if (Ch == TEXT('#'))
        {
            FIrToken Token;
            Token.Type = EIrTokenType::Comment;
            Token.Text = Line.Mid(Pos);
            Token.Position = Pos;
            Tokens.Add(MoveTemp(Token));
            break;
        }

        if (Ch == TEXT('"'))
        {
            ReadString(Line, Pos, Tokens);
            continue;
        }

        if (Ch == TEXT('%') || Ch == TEXT('$') || Ch == TEXT('@'))
        {
            ReadSigilToken(Line, Pos, Tokens);
            continue;
        }

        if (Ch == TEXT('-'))
        {
            if (Pos + 1 < Line.Len() && Line[Pos + 1] == TEXT('>'))
            {
                FIrToken Token;
                Token.Type = EIrTokenType::Arrow;
                Token.Text = TEXT("->");
                Token.Position = Pos;
                Tokens.Add(MoveTemp(Token));
                Pos += 2;
                continue;
            }
            if (Pos + 1 < Line.Len() && FChar::IsDigit(Line[Pos + 1]))
            {
                ReadNumber(Line, Pos, Tokens);
                continue;
            }
        }

        if (FChar::IsDigit(Ch))
        {
            ReadNumber(Line, Pos, Tokens);
            continue;
        }

        if (FChar::IsAlpha(Ch) || Ch == TEXT('_'))
        {
            ReadWord(Line, Pos, Grammar, Tokens);
            continue;
        }

        FIrToken Token;
        Token.Position = Pos;
        Token.Text = FString(1, &Ch);

        switch (Ch)
        {
        case TEXT('='): Token.Type = EIrTokenType::Equals; break;
        case TEXT('('): Token.Type = EIrTokenType::OpenParen; break;
        case TEXT(')'): Token.Type = EIrTokenType::CloseParen; break;
        case TEXT('['): Token.Type = EIrTokenType::OpenBracket; break;
        case TEXT(']'): Token.Type = EIrTokenType::CloseBracket; break;
        case TEXT('<'): Token.Type = EIrTokenType::OpenAngle; break;
        case TEXT('>'): Token.Type = EIrTokenType::CloseAngle; break;
        case TEXT(','): Token.Type = EIrTokenType::Comma; break;
        case TEXT(':'): Token.Type = EIrTokenType::Colon; break;
        default:        Token.Type = EIrTokenType::Unknown; break;
        }

        Tokens.Add(MoveTemp(Token));
        ++Pos;
    }

    return Tokens;
}
