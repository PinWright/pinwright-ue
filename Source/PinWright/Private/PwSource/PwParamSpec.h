// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "PwValue.h"

enum class EPwParamType : uint8
{
    Number,
    Integer,
    Bool,
    String,
    Enum,
    Vector2,
    Vector3,
    Vector4,
    NumberList,
    PointList2,
    PointList3,
    PointList4,
    FrameList,
    HarmonicList
};

struct FPwParamSpec
{
    FString Name;
    EPwParamType Type = EPwParamType::Number;
    bool bRequired = false;
    FString Default;
    FString Description;
    TArray<FString> AllowedValues;
    bool bHasRange = false;
    double MinValue = 0.0;
    double MaxValue = 0.0;
};

inline const TCHAR* PwParamTypeToString(EPwParamType Type)
{
    switch (Type)
    {
    case EPwParamType::Number:     return TEXT("number");
    case EPwParamType::Integer:    return TEXT("integer");
    case EPwParamType::Bool:       return TEXT("boolean");
    case EPwParamType::String:     return TEXT("string");
    case EPwParamType::Enum:       return TEXT("enum");
    case EPwParamType::Vector2:    return TEXT("vector2");
    case EPwParamType::Vector3:    return TEXT("vector3");
    case EPwParamType::Vector4:    return TEXT("vector4");
    case EPwParamType::NumberList: return TEXT("number_list");
    case EPwParamType::PointList2: return TEXT("point_list2");
    case EPwParamType::PointList3: return TEXT("point_list3");
    case EPwParamType::PointList4: return TEXT("point_list4");
    case EPwParamType::FrameList:  return TEXT("frame_list");
    case EPwParamType::HarmonicList: return TEXT("harmonic_list");
    default:                        return TEXT("unknown");
    }
}

inline const TCHAR* PwParamTypeTupleShape(EPwParamType Type)
{
    switch (Type)
    {
    case EPwParamType::FrameList:    return TEXT(" (x, y, z, roll, pitch, yaw)");
    case EPwParamType::HarmonicList: return TEXT(" (order, amplitude, phase)");
    default:                         return TEXT("");
    }
}

inline const TCHAR* PwValueTypeToString(EPwValueType Type)
{
    switch (Type)
    {
    case EPwValueType::Number:     return TEXT("a number");
    case EPwValueType::Tuple:      return TEXT("a tuple");
    case EPwValueType::TupleList:  return TEXT("a list of tuples");
    case EPwValueType::String:     return TEXT("a quoted string");
    case EPwValueType::Identifier: return TEXT("a bare identifier");
    case EPwValueType::None:       return TEXT("no value");
    default:                       return TEXT("something else");
    }
}
