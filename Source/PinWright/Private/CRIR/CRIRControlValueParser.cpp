// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRControlValueParser.h"

#include "Compat/EngineVersionCompat.h"
#include "IrCore/IrTextUtils.h"
#include "EulerTransform.h"
#include "TransformNoScale.h"

namespace
{
const TMap<FString, ERigControlType>& GetPrefixMap()
{
    static const TMap<FString, ERigControlType> Map = {
        { TEXT("bool"),               ERigControlType::Bool },
        { TEXT("float"),              ERigControlType::Float },
        { TEXT("int"),                ERigControlType::Integer },
        { TEXT("vector2d"),           ERigControlType::Vector2D },
        { TEXT("position"),           ERigControlType::Position },
        { TEXT("scale"),              ERigControlType::Scale },
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        // ERigControlType::ScaleFloat was added in UE 5.4; absent on 5.3.
        { TEXT("scale_float"),        ERigControlType::ScaleFloat },
#endif
        { TEXT("rotator"),            ERigControlType::Rotator },
        { TEXT("transform"),          ERigControlType::Transform },
        { TEXT("transform_no_scale"), ERigControlType::TransformNoScale },
        { TEXT("euler_transform"),    ERigControlType::EulerTransform },
    };
    return Map;
}

bool TryStripParens(const FString& Tuple, FString& OutInner)
{
    FString Trimmed = Tuple;
    Trimmed.TrimStartAndEndInline();
    if (!Trimmed.StartsWith(TEXT("(")) || !Trimmed.EndsWith(TEXT(")")))
    {
        return false;
    }
    OutInner = Trimmed.Mid(1, Trimmed.Len() - 2);
    return true;
}

bool ParseFloatTupleNWithError(const FString& Tuple, int32 ExpectedCount, TArray<double>& Out, FString& OutError)
{
    Out.Reset();
    FString Inner;
    if (!TryStripParens(Tuple, Inner))
    {
        OutError = FString::Printf(TEXT("expected (...) tuple, got '%s'"), *Tuple);
        return false;
    }
    TArray<FString> Parts = FIrTextUtils::SmartSplit(Inner, TEXT(','));
    if (Parts.Num() != ExpectedCount)
    {
        OutError = FString::Printf(TEXT("expected %d components in '%s', got %d"),
            ExpectedCount, *Tuple, Parts.Num());
        return false;
    }
    for (const FString& Part : Parts)
    {
        if (Part.IsEmpty())
        {
            OutError = FString::Printf(TEXT("empty component in '%s'"), *Tuple);
            return false;
        }
        Out.Add(FCString::Atod(*Part));
    }
    return true;
}

bool ParseKeywordArgs(const TArray<FString>& Parts, TMap<FString, FString>& OutKW, FString& OutError)
{
    for (const FString& Part : Parts)
    {
        const int32 EqIdx = Part.Find(TEXT("="));
        if (EqIdx == INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("expected key=value, got '%s'"), *Part);
            return false;
        }
        const FString Key = Part.Left(EqIdx).TrimEnd().ToLower();
        const FString Value = Part.Mid(EqIdx + 1).TrimStart();
        OutKW.Add(Key, Value);
    }
    return true;
}

bool ParseBoolLiteral(const FString& Text, bool& Out)
{
    const FString Lower = Text.ToLower();
    if (Lower == TEXT("true") || Lower == TEXT("1"))  { Out = true;  return true; }
    if (Lower == TEXT("false") || Lower == TEXT("0")) { Out = false; return true; }
    return false;
}
} // namespace

bool FCRIRControlValueParser::TryParseDoubleTuple(const FString& Tuple, int32 ExpectedCount, TArray<double>& Out)
{
    FString Discard;
    return ParseFloatTupleNWithError(Tuple, ExpectedCount, Out, Discard);
}

bool FCRIRControlValueParser::TryParseKeyValueTuple(const FString& Tuple, TMap<FString, FString>& Out)
{
    Out.Reset();
    FString Inner;
    if (!TryStripParens(Tuple, Inner))
    {
        Inner = Tuple;
        Inner.TrimStartAndEndInline();
    }
    TArray<FString> Parts = FIrTextUtils::SmartSplit(Inner, TEXT(','));
    FString Discard;
    return ParseKeywordArgs(Parts, Out, Discard);
}

const TCHAR* FCRIRControlValueParser::PrefixForType(ERigControlType Type)
{
    switch (Type)
    {
    case ERigControlType::Bool:             return TEXT("bool");
    case ERigControlType::Float:            return TEXT("float");
    case ERigControlType::Integer:          return TEXT("int");
    case ERigControlType::Vector2D:         return TEXT("vector2d");
    case ERigControlType::Position:         return TEXT("position");
    case ERigControlType::Scale:            return TEXT("scale");
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    case ERigControlType::ScaleFloat:       return TEXT("scale_float");
#endif
    case ERigControlType::Rotator:          return TEXT("rotator");
    case ERigControlType::Transform:        return TEXT("transform");
    case ERigControlType::TransformNoScale: return TEXT("transform_no_scale");
    case ERigControlType::EulerTransform:   return TEXT("euler_transform");
    default:
        checkNoEntry();
        return TEXT("");
    }
}

bool FCRIRControlValueParser::TryResolvePrefix(const FString& Prefix, ERigControlType& OutType)
{
    if (const ERigControlType* Found = GetPrefixMap().Find(Prefix.ToLower()))
    {
        OutType = *Found;
        return true;
    }
    return false;
}

bool FCRIRControlValueParser::Parse(
    const FString& Literal,
    ERigControlType ExpectedType,
    FRigControlValue& OutValue,
    FString& OutError)
{
    ECRIRControlValueParseError Cause;
    return ParseWithCause(Literal, ExpectedType, OutValue, OutError, Cause);
}

bool FCRIRControlValueParser::ParseWithCause(
    const FString& Literal,
    ERigControlType ExpectedType,
    FRigControlValue& OutValue,
    FString& OutError,
    ECRIRControlValueParseError& OutCause)
{
    OutError.Reset();
    OutCause = ECRIRControlValueParseError::Other;

    FString Trimmed = Literal;
    Trimmed.TrimStartAndEndInline();
    const int32 OpenIdx = Trimmed.Find(TEXT("("));
    if (OpenIdx == INDEX_NONE)
    {
        OutError = FString::Printf(TEXT("expected '<prefix>(...)', got '%s'"), *Literal);
        return false;
    }
    const FString Prefix = Trimmed.Left(OpenIdx).TrimEnd();
    ERigControlType ResolvedType;
    if (!TryResolvePrefix(Prefix, ResolvedType))
    {
        OutError = FString::Printf(TEXT("unknown value prefix '%s'"), *Prefix);
        return false;
    }
    if (ResolvedType != ExpectedType)
    {
        OutError = FString::Printf(TEXT("value prefix '%s' does not match type=%s"),
            *Prefix, PrefixForType(ExpectedType));
        OutCause = ECRIRControlValueParseError::PrefixTypeMismatch;
        return false;
    }

    const int32 CloseIdx = FIrTextUtils::FindMatchingChar(Trimmed, OpenIdx, TEXT('('), TEXT(')'));
    if (CloseIdx == INDEX_NONE)
    {
        OutError = FString::Printf(TEXT("unmatched '(' in '%s'"), *Literal);
        return false;
    }
    const FString ArgsText = Trimmed.Mid(OpenIdx + 1, CloseIdx - OpenIdx - 1);
    const TArray<FString> Parts = FIrTextUtils::SmartSplit(ArgsText, TEXT(','));

    switch (ExpectedType)
    {
    case ERigControlType::Bool:
    {
        if (Parts.Num() != 1)
        {
            OutError = TEXT("bool(...) takes one positional arg");
            return false;
        }
        bool B;
        if (!ParseBoolLiteral(Parts[0], B))
        {
            OutError = FString::Printf(TEXT("bool() arg '%s' is not true/false/0/1"), *Parts[0]);
            return false;
        }
        OutValue = FRigControlValue::Make<bool>(B);
        return true;
    }
    case ERigControlType::Float:
    {
        if (Parts.Num() != 1)
        {
            OutError = TEXT("float(...) takes one positional arg");
            return false;
        }
        OutValue = FRigControlValue::Make<float>(static_cast<float>(FCString::Atod(*Parts[0])));
        return true;
    }
    case ERigControlType::Integer:
    {
        if (Parts.Num() != 1)
        {
            OutError = TEXT("int(...) takes one positional arg");
            return false;
        }
        OutValue = FRigControlValue::Make<int32>(FCString::Atoi(*Parts[0]));
        return true;
    }
    case ERigControlType::Vector2D:
    {
        if (Parts.Num() != 2)
        {
            OutError = TEXT("vector2d(...) takes two positional args");
            return false;
        }
        const FVector3f V(
            static_cast<float>(FCString::Atod(*Parts[0])),
            static_cast<float>(FCString::Atod(*Parts[1])),
            0.f);
        OutValue = FRigControlValue::Make<FVector3f>(V);
        return true;
    }
    case ERigControlType::Position:
    case ERigControlType::Scale:
    {
        if (Parts.Num() != 3)
        {
            OutError = TEXT("position/scale(...) takes three positional args");
            return false;
        }
        const FVector3f V(
            static_cast<float>(FCString::Atod(*Parts[0])),
            static_cast<float>(FCString::Atod(*Parts[1])),
            static_cast<float>(FCString::Atod(*Parts[2])));
        OutValue = FRigControlValue::Make<FVector3f>(V);
        return true;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    case ERigControlType::ScaleFloat:
    {
        if (Parts.Num() != 1)
        {
            OutError = TEXT("scale_float(...) takes one positional arg");
            return false;
        }
        OutValue = FRigControlValue::Make<float>(static_cast<float>(FCString::Atod(*Parts[0])));
        return true;
    }
#endif
    case ERigControlType::Rotator:
    {
        TMap<FString, FString> KW;
        if (!ParseKeywordArgs(Parts, KW, OutError))
        {
            return false;
        }
        const FString* P = KW.Find(TEXT("p"));
        const FString* Y = KW.Find(TEXT("y"));
        const FString* R = KW.Find(TEXT("r"));
        if (!P || !Y || !R)
        {
            OutError = TEXT("rotator(...) requires p=, y=, r=");
            return false;
        }
        // FRigControlValue stores rotator as FVector3f euler (X=Pitch, Y=Yaw, Z=Roll)
        const FVector3f V(
            static_cast<float>(FCString::Atod(**P)),
            static_cast<float>(FCString::Atod(**Y)),
            static_cast<float>(FCString::Atod(**R)));
        OutValue = FRigControlValue::Make<FVector3f>(V);
        return true;
    }
    case ERigControlType::Transform:
    {
        TMap<FString, FString> KW;
        if (!ParseKeywordArgs(Parts, KW, OutError))
        {
            return false;
        }
        const FString* Loc = KW.Find(TEXT("loc"));
        const FString* Rot = KW.Find(TEXT("rot"));
        const FString* Scl = KW.Find(TEXT("scale"));
        if (!Loc || !Rot || !Scl)
        {
            OutError = TEXT("transform(...) requires loc=, rot=, scale=");
            return false;
        }
        TArray<double> LocV, RotV, SclV;
        FString Err;
        if (!ParseFloatTupleNWithError(*Loc, 3, LocV, Err)) { OutError = FString::Printf(TEXT("loc: %s"), *Err); return false; }
        if (!ParseFloatTupleNWithError(*Rot, 3, RotV, Err)) { OutError = FString::Printf(TEXT("rot: %s"), *Err); return false; }
        if (!ParseFloatTupleNWithError(*Scl, 3, SclV, Err)) { OutError = FString::Printf(TEXT("scale: %s"), *Err); return false; }
        FTransform Xf(
            FRotator(RotV[0], RotV[1], RotV[2]).Quaternion(),
            FVector(LocV[0], LocV[1], LocV[2]),
            FVector(SclV[0], SclV[1], SclV[2]));
        OutValue = FRigControlValue::Make<FTransform>(Xf);
        return true;
    }
    case ERigControlType::TransformNoScale:
    {
        TMap<FString, FString> KW;
        if (!ParseKeywordArgs(Parts, KW, OutError))
        {
            return false;
        }
        const FString* Loc = KW.Find(TEXT("loc"));
        const FString* Rot = KW.Find(TEXT("rot"));
        if (!Loc || !Rot)
        {
            OutError = TEXT("transform_no_scale(...) requires loc=, rot=");
            return false;
        }
        TArray<double> LocV, RotV;
        FString Err;
        if (!ParseFloatTupleNWithError(*Loc, 3, LocV, Err)) { OutError = FString::Printf(TEXT("loc: %s"), *Err); return false; }
        if (!ParseFloatTupleNWithError(*Rot, 3, RotV, Err)) { OutError = FString::Printf(TEXT("rot: %s"), *Err); return false; }
        FTransformNoScale Xf;
        Xf.Location = FVector(LocV[0], LocV[1], LocV[2]);
        Xf.Rotation = FRotator(RotV[0], RotV[1], RotV[2]).Quaternion();
        OutValue = FRigControlValue::Make<FTransformNoScale>(Xf);
        return true;
    }
    case ERigControlType::EulerTransform:
    {
        TMap<FString, FString> KW;
        if (!ParseKeywordArgs(Parts, KW, OutError))
        {
            return false;
        }
        const FString* Loc = KW.Find(TEXT("loc"));
        const FString* Rot = KW.Find(TEXT("rot"));
        const FString* Scl = KW.Find(TEXT("scale"));
        if (!Loc || !Rot || !Scl)
        {
            OutError = TEXT("euler_transform(...) requires loc=, rot=, scale=");
            return false;
        }
        TArray<double> LocV, RotV, SclV;
        FString Err;
        if (!ParseFloatTupleNWithError(*Loc, 3, LocV, Err)) { OutError = FString::Printf(TEXT("loc: %s"), *Err); return false; }
        if (!ParseFloatTupleNWithError(*Rot, 3, RotV, Err)) { OutError = FString::Printf(TEXT("rot: %s"), *Err); return false; }
        if (!ParseFloatTupleNWithError(*Scl, 3, SclV, Err)) { OutError = FString::Printf(TEXT("scale: %s"), *Err); return false; }
        FEulerTransform Xf;
        Xf.Location = FVector(LocV[0], LocV[1], LocV[2]);
        Xf.Rotation = FRotator(RotV[0], RotV[1], RotV[2]);
        Xf.Scale = FVector(SclV[0], SclV[1], SclV[2]);
        OutValue = FRigControlValue::Make<FEulerTransform>(Xf);
        return true;
    }
    default:
        OutError = TEXT("unsupported control type");
        return false;
    }
}
