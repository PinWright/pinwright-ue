// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

enum class EPwTokenType : uint8
{
    Identifier,
    Number,
    String,
    Equals,
    Comma,
    OpenBrace,
    CloseBrace,
    OpenParen,
    CloseParen,
    OpenBracket,
    CloseBracket,
    Newline,
    EndOfFile,
    Unknown
};

struct FPwToken
{
    EPwTokenType Type = EPwTokenType::Unknown;
    FString Text;      // String carries the DECODED value; Newline and EndOfFile are empty.
    int32 Line = 0;    // 1-based; 0 means not set.
    int32 Column = 0;
};

inline const TCHAR* PwTokenTypeToString(EPwTokenType Type)
{
    switch (Type)
    {
    case EPwTokenType::Identifier:   return TEXT("identifier");
    case EPwTokenType::Number:       return TEXT("number");
    case EPwTokenType::String:       return TEXT("string");
    case EPwTokenType::Equals:       return TEXT("'='");
    case EPwTokenType::Comma:        return TEXT("','");
    case EPwTokenType::OpenBrace:    return TEXT("'{'");
    case EPwTokenType::CloseBrace:   return TEXT("'}'");
    case EPwTokenType::OpenParen:    return TEXT("'('");
    case EPwTokenType::CloseParen:   return TEXT("')'");
    case EPwTokenType::OpenBracket:  return TEXT("'['");
    case EPwTokenType::CloseBracket: return TEXT("']'");
    case EPwTokenType::Newline:      return TEXT("end of line");
    case EPwTokenType::EndOfFile:    return TEXT("end of file");
    default:                         return TEXT("unknown token");
    }
}
