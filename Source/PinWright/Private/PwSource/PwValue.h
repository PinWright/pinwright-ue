// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

enum class EPwValueType : uint8
{
    None,
    Number,
    Tuple,
    TupleList,
    String,
    Identifier
};

struct FPwValue
{
    EPwValueType Type = EPwValueType::None;
    double Number = 0.0;
    TArray<double> Tuple;
    TArray<TArray<double>> TupleList;
    FString Text;
    int32 Line = 0;
    int32 Column = 0;
};

struct FPwOp
{
    FString OpName;
    TMap<FString, FPwValue> Params;
    TArray<FPwOp> Children;
    int32 Line = 0;
    int32 Column = 0;
};
