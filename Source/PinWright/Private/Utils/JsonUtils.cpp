// Copyright (c) 2026 Alexander Penkin. MIT License.

// JSON field helper utilities for PinWright
#include "Utils/JsonUtils.h"

#include "Compat/JsonKeyCompat.h"
#include "Containers/StringConv.h"
#include "Handlers/ParamTypeCheck.h"
#include "Misc/DefaultValueHelper.h"
#include "UObject/Object.h"
#include "UObject/Class.h"

// Internal helpers used only within this translation unit
namespace
{

// Read a vector from a JSON object field into an FVector.
// Supports object form {x, y, z} or array form [x, y, z].
void ReadVectorFieldImpl(const TSharedPtr<FJsonObject>& Obj,
                         const TCHAR* FieldName, FVector& Out,
                         const FVector& Default)
{
    if (!Obj.IsValid())
    {
        Out = Default;
        return;
    }
    const TSharedPtr<FJsonObject>* FieldObj = nullptr;
    if (Obj->TryGetObjectField(FieldName, FieldObj) && FieldObj &&
        (*FieldObj).IsValid())
    {
        double X = Default.X, Y = Default.Y, Z = Default.Z;
        if (!(*FieldObj)->TryGetNumberField(TEXT("x"), X))
            (*FieldObj)->TryGetNumberField(TEXT("X"), X);
        if (!(*FieldObj)->TryGetNumberField(TEXT("y"), Y))
            (*FieldObj)->TryGetNumberField(TEXT("Y"), Y);
        if (!(*FieldObj)->TryGetNumberField(TEXT("z"), Z))
            (*FieldObj)->TryGetNumberField(TEXT("Z"), Z);
        Out = FVector(X, Y, Z);
        return;
    }
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Obj->TryGetArrayField(FieldName, Arr) && Arr && Arr->Num() >= 3)
    {
        Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(),
                      (*Arr)[2]->AsNumber());
        return;
    }
    Out = Default;
}

// Read a rotator from a JSON object field into an FRotator.
// Supports object form {pitch, yaw, roll} or array form [pitch, yaw, roll].
void ReadRotatorFieldImpl(const TSharedPtr<FJsonObject>& Obj,
                          const TCHAR* FieldName, FRotator& Out,
                          const FRotator& Default)
{
    if (!Obj.IsValid())
    {
        Out = Default;
        return;
    }
    const TSharedPtr<FJsonObject>* FieldObj = nullptr;
    if (Obj->TryGetObjectField(FieldName, FieldObj) && FieldObj &&
        (*FieldObj).IsValid())
    {
        double Pitch = Default.Pitch, Yaw = Default.Yaw, Roll = Default.Roll;
        if (!(*FieldObj)->TryGetNumberField(TEXT("pitch"), Pitch))
            (*FieldObj)->TryGetNumberField(TEXT("Pitch"), Pitch);
        if (!(*FieldObj)->TryGetNumberField(TEXT("yaw"), Yaw))
            (*FieldObj)->TryGetNumberField(TEXT("Yaw"), Yaw);
        if (!(*FieldObj)->TryGetNumberField(TEXT("roll"), Roll))
            (*FieldObj)->TryGetNumberField(TEXT("Roll"), Roll);
        Out = FRotator(Pitch, Yaw, Roll);
        return;
    }
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Obj->TryGetArrayField(FieldName, Arr) && Arr && Arr->Num() >= 3)
    {
        Out = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(),
                       (*Arr)[2]->AsNumber());
        return;
    }
    Out = Default;
}

bool ParseStrictNumberString(const FString& Raw, double& OutValue)
{
    if (!PinWrightParamTypes::PinWrightIsStrictNumericLiteral(Raw)
        || !FDefaultValueHelper::IsStringValidFloat(Raw)
        || !LexTryParseString(OutValue, *Raw)
        || !FMath::IsFinite(OutValue))
    {
        return false;
    }
    return true;
}

bool ParseStrictSignedIntegerString(const FString& Raw, int64& OutValue)
{
    if (!PinWrightParamTypes::PinWrightIsStrictNumericLiteral(Raw)
        || !FDefaultValueHelper::IsStringValidInteger(Raw))
    {
        return false;
    }

    int32 Index = 0;
    bool bNegative = false;
    if (Raw[Index] == TEXT('+') || Raw[Index] == TEXT('-'))
    {
        bNegative = Raw[Index] == TEXT('-');
        ++Index;
    }

    const uint64 PositiveLimit = static_cast<uint64>(TNumericLimits<int64>::Max());
    const uint64 NegativeLimit = PositiveLimit + 1;
    const uint64 Limit = bNegative ? NegativeLimit : PositiveLimit;
    uint64 Magnitude = 0;
    for (; Index < Raw.Len(); ++Index)
    {
        const uint64 Digit = static_cast<uint64>(Raw[Index] - TEXT('0'));
        if (Digit > 9 || Magnitude > (Limit - Digit) / 10)
        {
            return false;
        }
        Magnitude = Magnitude * 10 + Digit;
    }

    if (bNegative)
    {
        OutValue = Magnitude == NegativeLimit
            ? TNumericLimits<int64>::Min()
            : -static_cast<int64>(Magnitude);
    }
    else
    {
        OutValue = static_cast<int64>(Magnitude);
    }
    return true;
}

bool ParseStrictUnsignedIntegerString(const FString& Raw, uint64& OutValue)
{
    if (!PinWrightParamTypes::PinWrightIsStrictNumericLiteral(Raw)
        || !FDefaultValueHelper::IsStringValidInteger(Raw))
    {
        return false;
    }

    int32 Index = 0;
    if (Raw[Index] == TEXT('+'))
    {
        ++Index;
    }
    if (Index < Raw.Len() && Raw[Index] == TEXT('-'))
    {
        return false;
    }

    uint64 Magnitude = 0;
    for (; Index < Raw.Len(); ++Index)
    {
        const uint64 Digit = static_cast<uint64>(Raw[Index] - TEXT('0'));
        if (Digit > 9 || Magnitude > (TNumericLimits<uint64>::Max() - Digit) / 10)
        {
            return false;
        }
        Magnitude = Magnitude * 10 + Digit;
    }
    OutValue = Magnitude;
    return true;
}

} // anonymous namespace

void ReadVectorField(const TSharedPtr<FJsonObject>& Obj,
                     const TCHAR* FieldName, FVector& Out,
                     const FVector& Default)
{
    ReadVectorFieldImpl(Obj, FieldName, Out, Default);
}

void ReadRotatorField(const TSharedPtr<FJsonObject>& Obj,
                      const TCHAR* FieldName, FRotator& Out,
                      const FRotator& Default)
{
    ReadRotatorFieldImpl(Obj, FieldName, Out, Default);
}

FString GetJsonStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, const FString& Default)
{
    FString Value;
    if (Obj.IsValid() && Obj->TryGetStringField(Field, Value))
    {
        return Value;
    }
    return Default;
}

double GetJsonNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, double Default)
{
    double Value = Default;
    if (Obj.IsValid())
    {
        Obj->TryGetNumberField(Field, Value);
    }
    return Value;
}

bool GetJsonBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, bool Default)
{
    bool Value = Default;
    if (Obj.IsValid())
    {
        Obj->TryGetBoolField(Field, Value);
    }
    return Value;
}

int32 GetJsonIntField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, int32 Default)
{
    double Value = static_cast<double>(Default);
    if (Obj.IsValid())
    {
        Obj->TryGetNumberField(Field, Value);
    }
    return static_cast<int32>(Value);
}

bool TryParseStrictJsonNumber(const TSharedPtr<FJsonValue>& Value,
                              double& OutValue, FString& OutError)
{
    OutError.Empty();
    if (!Value.IsValid())
    {
        OutError = TEXT("Expected a finite numeric JSON value");
        return false;
    }

    if (Value->Type == EJson::Number)
    {
        OutValue = Value->AsNumber();
        if (FMath::IsFinite(OutValue))
        {
            return true;
        }
        OutError = TEXT("Numeric value must be finite");
        return false;
    }
    if (Value->Type == EJson::String && ParseStrictNumberString(Value->AsString(), OutValue))
    {
        return true;
    }

    OutError = TEXT("Expected a finite strict numeric literal");
    return false;
}

bool TryParseStrictJsonInteger(const TSharedPtr<FJsonValue>& Value,
                               int64 MinValue, int64 MaxValue,
                               int64& OutValue, FString& OutError)
{
    OutError.Empty();
    if (!Value.IsValid())
    {
        OutError = TEXT("Expected a finite integral JSON value");
        return false;
    }

    if (Value->Type == EJson::String)
    {
        if (!ParseStrictSignedIntegerString(Value->AsString(), OutValue))
        {
            OutError = TEXT("Expected a strict integral literal");
            return false;
        }
    }
    else if (Value->Type == EJson::Number)
    {
        const double Number = Value->AsNumber();
        // JSON numbers are doubles in UE. Refuse values outside the exactly representable
        // integer range instead of silently changing a large integer during the cast.
        constexpr double MaxExactlyRepresentableInteger = 9007199254740991.0;
        if (!FMath::IsFinite(Number)
            || FMath::Abs(Number) > MaxExactlyRepresentableInteger
            || FMath::TruncToDouble(Number) != Number
            || Number < static_cast<double>(MinValue)
            || Number > static_cast<double>(MaxValue))
        {
            OutError = TEXT("Expected a finite in-range integral JSON number");
            return false;
        }
        OutValue = static_cast<int64>(Number);
    }
    else
    {
        OutError = TEXT("Expected a JSON number or strict integral string");
        return false;
    }

    if (OutValue < MinValue || OutValue > MaxValue)
    {
        OutError = TEXT("Integral value is outside the property range");
        return false;
    }
    return true;
}

bool TryParseStrictJsonUnsignedInteger(const TSharedPtr<FJsonValue>& Value,
                                       uint64 MaxValue,
                                       uint64& OutValue, FString& OutError)
{
    OutError.Empty();
    if (!Value.IsValid())
    {
        OutError = TEXT("Expected a finite unsigned integral JSON value");
        return false;
    }

    if (Value->Type == EJson::String)
    {
        if (!ParseStrictUnsignedIntegerString(Value->AsString(), OutValue))
        {
            OutError = TEXT("Expected a strict unsigned integral literal");
            return false;
        }
    }
    else if (Value->Type == EJson::Number)
    {
        const double Number = Value->AsNumber();
        constexpr double MaxExactlyRepresentableInteger = 9007199254740991.0;
        if (!FMath::IsFinite(Number)
            || Number < 0.0
            || Number > MaxExactlyRepresentableInteger
            || FMath::TruncToDouble(Number) != Number)
        {
            OutError = TEXT("Expected a finite non-negative in-range integral JSON number");
            return false;
        }
        OutValue = static_cast<uint64>(Number);
    }
    else
    {
        OutError = TEXT("Expected a JSON number or strict unsigned integral string");
        return false;
    }

    if (OutValue > MaxValue)
    {
        OutError = TEXT("Unsigned integral value is outside the property range");
        return false;
    }
    return true;
}

bool TryParseStrictJsonBoolean(const TSharedPtr<FJsonValue>& Value,
                               bool& OutValue, FString& OutError)
{
    OutError.Empty();
    if (!Value.IsValid())
    {
        OutError = TEXT("Expected a boolean JSON value");
        return false;
    }
    if (Value->Type == EJson::Boolean)
    {
        OutValue = Value->AsBool();
        return true;
    }
    if (Value->Type == EJson::Number)
    {
        const double Number = Value->AsNumber();
        if (!FMath::IsFinite(Number))
        {
            OutError = TEXT("Boolean numeric value must be finite");
            return false;
        }
        OutValue = Number != 0.0;
        return true;
    }
    if (Value->Type != EJson::String)
    {
        OutError = TEXT("Expected a boolean, number, or boolean literal");
        return false;
    }

    const FString Raw = Value->AsString();
    if (!PinWrightParamTypes::PinWrightIsBooleanLiteral(Raw))
    {
        OutError = TEXT("Expected true/false, yes/no, on/off, or a strict numeric literal");
        return false;
    }
    if (Raw.Equals(TEXT("true"), ESearchCase::IgnoreCase)
        || Raw.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
        || Raw.Equals(TEXT("on"), ESearchCase::IgnoreCase))
    {
        OutValue = true;
        return true;
    }
    if (Raw.Equals(TEXT("false"), ESearchCase::IgnoreCase)
        || Raw.Equals(TEXT("no"), ESearchCase::IgnoreCase)
        || Raw.Equals(TEXT("off"), ESearchCase::IgnoreCase))
    {
        OutValue = false;
        return true;
    }

    double Number = 0.0;
    if (!ParseStrictNumberString(Raw, Number))
    {
        OutError = TEXT("Boolean numeric literal is invalid or non-finite");
        return false;
    }
    OutValue = Number != 0.0;
    return true;
}

FVector ExtractVectorField(const TSharedPtr<FJsonObject>& Source,
                           const TCHAR* FieldName,
                           const FVector& DefaultValue)
{
    FVector Parsed = DefaultValue;
    ReadVectorFieldImpl(Source, FieldName, Parsed, DefaultValue);
    return Parsed;
}

FRotator ExtractRotatorField(const TSharedPtr<FJsonObject>& Source,
                             const TCHAR* FieldName,
                             const FRotator& DefaultValue)
{
    FRotator Parsed = DefaultValue;
    ReadRotatorFieldImpl(Source, FieldName, Parsed, DefaultValue);
    return Parsed;
}

FVector ParseVectorFromJson(const TSharedPtr<FJsonObject>& JsonObj, const FString& FieldName, const FVector& Default)
{
    if (!JsonObj.IsValid() || !JsonObj->HasField(FieldName))
    {
        return Default;
    }

    const TSharedPtr<FJsonObject>* VecObj = nullptr;
    if (JsonObj->TryGetObjectField(FieldName, VecObj) && VecObj && VecObj->IsValid())
    {
        double X = 0.0, Y = 0.0, Z = 0.0;
        (*VecObj)->TryGetNumberField(TEXT("x"), X);
        (*VecObj)->TryGetNumberField(TEXT("y"), Y);
        (*VecObj)->TryGetNumberField(TEXT("z"), Z);
        return FVector(X, Y, Z);
    }

    return Default;
}

FRotator ParseRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObj, const FString& FieldName, const FRotator& Default)
{
    if (!JsonObj.IsValid() || !JsonObj->HasField(FieldName))
    {
        return Default;
    }

    const TSharedPtr<FJsonObject>* RotObj = nullptr;
    if (JsonObj->TryGetObjectField(FieldName, RotObj) && RotObj && RotObj->IsValid())
    {
        double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
        (*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
        (*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
        return FRotator(Pitch, Yaw, Roll);
    }

    return Default;
}

TSharedPtr<FJsonValue> JsonVec2(const FVector2f& Value)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("x"), Value.X);
    Obj->SetNumberField(TEXT("y"), Value.Y);
    return MakeShared<FJsonValueObject>(Obj);
}

TSharedPtr<FJsonValue> JsonHalf(FFloat16 Value)
{
    return MakeShared<FJsonValueNumber>(Value.GetFloat());
}

TSharedPtr<FJsonValue> JsonHalfVec2(FFloat16 X, FFloat16 Y)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetField(TEXT("x"), JsonHalf(X));
    Obj->SetField(TEXT("y"), JsonHalf(Y));
    return MakeShared<FJsonValueObject>(Obj);
}

TSharedPtr<FJsonValue> JsonHalfVec3(FFloat16 X, FFloat16 Y, FFloat16 Z)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetField(TEXT("x"), JsonHalf(X));
    Obj->SetField(TEXT("y"), JsonHalf(Y));
    Obj->SetField(TEXT("z"), JsonHalf(Z));
    return MakeShared<FJsonValueObject>(Obj);
}

TSharedPtr<FJsonValue> JsonHalfVec4(FFloat16 X, FFloat16 Y, FFloat16 Z, FFloat16 W)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetField(TEXT("x"), JsonHalf(X));
    Obj->SetField(TEXT("y"), JsonHalf(Y));
    Obj->SetField(TEXT("z"), JsonHalf(Z));
    Obj->SetField(TEXT("w"), JsonHalf(W));
    return MakeShared<FJsonValueObject>(Obj);
}

bool ExtractVector2fField(const TSharedPtr<FJsonObject>& Source,
                          const FString& FieldName,
                          FVector2f& Out)
{
    if (!Source.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* FieldObj = nullptr;
    if (Source->TryGetObjectField(FieldName, FieldObj) && FieldObj && (*FieldObj).IsValid())
    {
        double X = 0.0;
        double Y = 0.0;
        if (!(*FieldObj)->TryGetNumberField(TEXT("x"), X))
            (*FieldObj)->TryGetNumberField(TEXT("X"), X);
        if (!(*FieldObj)->TryGetNumberField(TEXT("y"), Y))
            (*FieldObj)->TryGetNumberField(TEXT("Y"), Y);
        Out = FVector2f(static_cast<float>(X), static_cast<float>(Y));
        return true;
    }
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Source->TryGetArrayField(FieldName, Arr) && Arr && Arr->Num() >= 2)
    {
        Out = FVector2f(
            static_cast<float>((*Arr)[0]->AsNumber()),
            static_cast<float>((*Arr)[1]->AsNumber()));
        return true;
    }
    return false;
}

bool ExtractFloat16Field(const TSharedPtr<FJsonObject>& Source,
                         const FString& FieldName,
                         FFloat16& Out)
{
    if (!Source.IsValid())
    {
        return false;
    }
    double Value = 0.0;
    if (!Source->TryGetNumberField(FieldName, Value))
    {
        return false;
    }
    Out.Set(static_cast<float>(Value));
    return true;
}

bool RejectUnknownKeys(const TSharedPtr<FJsonObject>& Obj,
                       const TArray<FString>& Allowed,
                       TArray<FString>& OutUnknown,
                       ERejectUnknownKeysMode Mode,
                       ESearchCase::Type SearchCase)
{
    OutUnknown.Reset();
    if (!Obj.IsValid())
    {
        return true;
    }

    for (const auto& Pair : Obj->Values)
    {
        const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
        const bool bKnown = Allowed.ContainsByPredicate([&Key, SearchCase](const FString& Candidate)
        {
            return Candidate.Equals(Key, SearchCase);
        });
        if (bKnown)
        {
            continue;
        }

        OutUnknown.Add(Key);
        if (Mode == ERejectUnknownKeysMode::First)
        {
            return false;
        }
    }

    if (Mode == ERejectUnknownKeysMode::AllSorted)
    {
        OutUnknown.Sort();
    }
    return OutUnknown.Num() == 0;
}

TArray<FString> ExtractTopLevelJsonObjects(const FString& In)
{
    TArray<FString> Results;
    int32 Depth = 0;
    int32 Start = INDEX_NONE;
    for (int32 i = 0; i < In.Len(); ++i)
    {
        const TCHAR C = In[i];
        if (C == '{')
        {
            if (Depth == 0)
                Start = i;
            Depth++;
        }
        else if (C == '}')
        {
            Depth--;
            if (Depth == 0 && Start != INDEX_NONE)
            {
                Results.Add(In.Mid(Start, i - Start + 1));
                Start = INDEX_NONE;
            }
        }
    }
    return Results;
}

FString HexifyUtf8(const FString& In)
{
    FTCHARToUTF8 Converter(*In);
    const uint8* Bytes = reinterpret_cast<const uint8*>(Converter.Get());
    int32 Len = Converter.Length();
    FString Hex;
    Hex.Reserve(Len * 2);
    for (int32 i = 0; i < Len; ++i)
    {
        Hex += FString::Printf(TEXT("%02x"), Bytes[i]);
    }
    return Hex;
}

TSharedPtr<FJsonObject> EmitObjectRef(const UObject* Object)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    if (!Object)
    {
        return Json;
    }
    Json->SetStringField(TEXT("objectPath"), Object->GetPathName());
    Json->SetStringField(TEXT("className"), Object->GetClass()->GetName());
    return Json;
}

TArray<TSharedPtr<FJsonValue>> EmitStringArray(const TArray<FString>& Strings)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    Out.Reserve(Strings.Num());
    for (const FString& S : Strings)
    {
        Out.Add(MakeShared<FJsonValueString>(S));
    }
    return Out;
}
