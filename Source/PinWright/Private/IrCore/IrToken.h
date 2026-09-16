// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

enum class EIrTokenType : uint8
{
    Keyword,
    Identifier,
    PercentRef,
    DollarRef,
    LabelDef,
    LabelRef,
    Equals,
    Arrow,
    OpenParen,
    CloseParen,
    OpenBracket,
    CloseBracket,
    OpenAngle,
    CloseAngle,
    Comma,
    Colon,
    StringLiteral,
    NumberLiteral,
    Comment,
    Unknown
};

struct FIrToken
{
    EIrTokenType Type = EIrTokenType::Unknown;
    FString Text;
    int32 Position = 0;
};
