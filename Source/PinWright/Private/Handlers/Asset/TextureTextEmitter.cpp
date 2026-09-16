// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/TextureTextEmitter.h"

#include "AssetTextEmitterHelpers.h"
#include "Dom/JsonValue.h"

namespace
{
    using AssetTextEmitterHelpers::FormatNumber;
    using AssetTextEmitterHelpers::AppendIndent;
    using AssetTextEmitterHelpers::AppendScalar;
    using AssetTextEmitterHelpers::AppendOptionalString;
    using AssetTextEmitterHelpers::AppendOptionalNumber;

    void AppendTextureOptionalBool(FString& Out, int32 Indent, const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        bool bValue = false;
        if (Object.IsValid() && Object->TryGetBoolField(Field, bValue))
        {
            AppendScalar(Out, Indent, Field, bValue ? TEXT("true") : TEXT("false"));
        }
    }

    FString FormatDimensionObject(const TSharedPtr<FJsonObject>& Object)
    {
        TArray<FString> Parts;
        const TCHAR* Fields[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("slices") };
        for (const TCHAR* Field : Fields)
        {
            double Value = 0.0;
            if (Object.IsValid() && Object->TryGetNumberField(Field, Value))
            {
                Parts.Add(FString::Printf(TEXT("%s=%s"), Field, *FormatNumber(Value)));
            }
        }
        return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(", ")));
    }

    void AppendSize(FString& Out, const TSharedPtr<FJsonObject>& Root)
    {
        const TSharedPtr<FJsonObject>* Size = nullptr;
        if (Root->TryGetObjectField(TEXT("size"), Size) && Size && Size->IsValid())
        {
            AppendScalar(Out, 1, TEXT("size"), FormatDimensionObject(*Size));
        }
    }

    void AppendSource(FString& Out, const TSharedPtr<FJsonObject>& Root)
    {
        const TSharedPtr<FJsonObject>* Source = nullptr;
        if (!Root->TryGetObjectField(TEXT("source"), Source) || !Source || !Source->IsValid())
        {
            return;
        }

        AppendIndent(Out, 1);
        Out += TEXT("source {\n");
        AppendOptionalString(Out, 2, *Source, TEXT("sourceKind"));
        AppendScalar(Out, 2, TEXT("size"), FormatDimensionObject(*Source));
        AppendOptionalString(Out, 2, *Source, TEXT("format"));
        AppendOptionalString(Out, 2, *Source, TEXT("pixelFormat"));
        AppendIndent(Out, 1);
        Out += TEXT("}\n");
    }
}

namespace TextureTextEmitter
{

FString BuildText(const TSharedPtr<FJsonObject>& TextureJson)
{
    if (!TextureJson.IsValid())
    {
        return FString();
    }

    FString Out;
    Out += TEXT("texture {\n");
    AppendOptionalString(Out, 1, TextureJson, TEXT("kind"));
    AppendOptionalString(Out, 1, TextureJson, TEXT("textureClass"));
    AppendSize(Out, TextureJson);
    AppendOptionalNumber(Out, 1, TextureJson, TEXT("arraySize"));
    AppendOptionalString(Out, 1, TextureJson, TEXT("pixelFormat"));
    AppendOptionalString(Out, 1, TextureJson, TEXT("format"));
    AppendOptionalString(Out, 1, TextureJson, TEXT("compressionSettings"));
    AppendOptionalString(Out, 1, TextureJson, TEXT("lodGroup"));
    AppendTextureOptionalBool(Out, 1, TextureJson, TEXT("srgb"));
    AppendOptionalString(Out, 1, TextureJson, TEXT("mipGenSettings"));
    AppendTextureOptionalBool(Out, 1, TextureJson, TEXT("neverStream"));
    AppendSource(Out, TextureJson);
    Out += TEXT("}\n");
    return Out;
}

}
