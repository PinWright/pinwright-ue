// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRTypeGrammar.h"

namespace
{
const TMap<FString, EMGIRValueType>& GetTypeKeywords()
{
    static const TMap<FString, EMGIRValueType> Keywords = {
        { TEXT("float"), EMGIRValueType::Float1 },
        { TEXT("float1"), EMGIRValueType::Float1 },
        { TEXT("scalar"), EMGIRValueType::Float1 },
        { TEXT("float2"), EMGIRValueType::Float2 },
        { TEXT("vector2"), EMGIRValueType::Float2 },
        { TEXT("float3"), EMGIRValueType::Float3 },
        { TEXT("vector3"), EMGIRValueType::Float3 },
        { TEXT("color"), EMGIRValueType::Float3 },
        { TEXT("float4"), EMGIRValueType::Float4 },
        { TEXT("vector4"), EMGIRValueType::Float4 },
        { TEXT("texture2d"), EMGIRValueType::Texture2D },
        { TEXT("texture"), EMGIRValueType::Texture2D },
        { TEXT("materialattributes"), EMGIRValueType::MaterialAttributes },
        { TEXT("material_attributes"), EMGIRValueType::MaterialAttributes },
    };
    return Keywords;
}
}

bool FMGIRTypeGrammar::TryParseType(const FString& Text, EMGIRValueType& OutType)
{
    const FString Key = Text.TrimStartAndEnd().ToLower();
    if (const EMGIRValueType* Type = GetTypeKeywords().Find(Key))
    {
        OutType = *Type;
        return true;
    }

    OutType = EMGIRValueType::Unknown;
    return false;
}

FString FMGIRTypeGrammar::TypeToText(EMGIRValueType Type)
{
    switch (Type)
    {
    case EMGIRValueType::Float1: return TEXT("Float1");
    case EMGIRValueType::Float2: return TEXT("Float2");
    case EMGIRValueType::Float3: return TEXT("Float3");
    case EMGIRValueType::Float4: return TEXT("Float4");
    case EMGIRValueType::Texture2D: return TEXT("Texture2D");
    case EMGIRValueType::MaterialAttributes: return TEXT("MaterialAttributes");
    case EMGIRValueType::Unknown:
    default:
        return TEXT("Unknown");
    }
}

bool FMGIRTypeGrammar::IsKnownType(const FString& Text)
{
    EMGIRValueType IgnoredType = EMGIRValueType::Unknown;
    return TryParseType(Text, IgnoredType);
}
