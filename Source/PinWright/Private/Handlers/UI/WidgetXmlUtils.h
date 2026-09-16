// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared XML utilities for widget export/import handlers
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Compat/JsonKeyCompat.h"

namespace WidgetXmlHelpers
{

/** Escape a string for use as an XML attribute value. */
inline FString XmlEscapeAttribute(const FString& Raw)
{
    // Fast path: most property values (numbers, enums, identifiers) contain no special chars
    int32 Unused;
    if (!Raw.FindChar(TEXT('&'), Unused) && !Raw.FindChar(TEXT('<'), Unused)
        && !Raw.FindChar(TEXT('>'), Unused) && !Raw.FindChar(TEXT('"'), Unused))
    {
        return Raw;
    }
    FString Out = Raw;
    Out.ReplaceInline(TEXT("&"), TEXT("&amp;"));
    Out.ReplaceInline(TEXT("<"), TEXT("&lt;"));
    Out.ReplaceInline(TEXT(">"), TEXT("&gt;"));
    Out.ReplaceInline(TEXT("\""), TEXT("&quot;"));
    return Out;
}

/**
 * Sanitize a string for use as an XML attribute or element name.
 * Replaces any character outside [A-Za-z0-9_.-] with '_'.
 * Prepends '_' when the first character is a digit, '-', or '.' (XML forbids
 * these as NameStartChar). Fast-path returns the input unchanged when valid.
 */
inline FString SanitizeXmlName(const FString& Raw)
{
    if (Raw.IsEmpty())
        return Raw;

    bool bNeedsRewrite = false;
    for (TCHAR Ch : Raw)
    {
        if (!FChar::IsAlpha(Ch) && !FChar::IsDigit(Ch)
            && Ch != TEXT('_') && Ch != TEXT('.') && Ch != TEXT('-'))
        {
            bNeedsRewrite = true;
            break;
        }
    }

    // Also need rewrite if first char is a digit, '-', or '.' (invalid NameStartChar)
    const TCHAR First = Raw[0];
    if (FChar::IsDigit(First) || First == TEXT('-') || First == TEXT('.'))
        bNeedsRewrite = true;

    if (!bNeedsRewrite)
        return Raw;

    FString Out;
    Out.Reserve(Raw.Len() + 1);
    for (TCHAR Ch : Raw)
    {
        if (FChar::IsAlpha(Ch) || FChar::IsDigit(Ch)
            || Ch == TEXT('_') || Ch == TEXT('.') || Ch == TEXT('-'))
        {
            Out.AppendChar(Ch);
        }
        else
        {
            Out.AppendChar(TEXT('_'));
        }
    }

    // Prepend '_' if the (possibly-substituted) first char is still an invalid NameStartChar
    if (!Out.IsEmpty())
    {
        const TCHAR OutFirst = Out[0];
        if (FChar::IsDigit(OutFirst) || OutFirst == TEXT('-') || OutFirst == TEXT('.'))
        {
            Out = FString(TEXT("_")) + Out;
        }
    }
    return Out;
}

/** Reverse XML entity escaping. UE's FXmlFile does NOT decode entities. */
inline FString XmlUnescapeAttribute(const FString& Escaped)
{
    FString Out = Escaped;
    Out.ReplaceInline(TEXT("&quot;"), TEXT("\""));
    Out.ReplaceInline(TEXT("&gt;"), TEXT(">"));
    Out.ReplaceInline(TEXT("&lt;"), TEXT("<"));
    Out.ReplaceInline(TEXT("&amp;"), TEXT("&"));
    return Out;
}

/**
 * Strip both the leading 'U' (UClass naming) and the trailing '_C' (BPGC suffix)
 * so callers get the human-readable short name. Symmetric with the import side
 * which retries `+"_C"` on lookup (see ResolveWidgetClassFromTag).
 * Example: "UTextBlock" -> "TextBlock"; "WBP_Foo_C" -> "WBP_Foo".
 */
inline FString StripClassPrefix(const FString& ClassName)
{
    FString Result = ClassName;
    if (Result.Len() >= 2
        && Result[0] == TEXT('U')
        && FChar::IsUpper(Result[1]))
    {
        Result = Result.RightChop(1);
    }
    Result.RemoveFromEnd(TEXT("_C"));
    return Result;
}

/** Convert a JSON value to a flat string suitable for an XML attribute. */
inline FString JsonValueToAttrString(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid() || Value->IsNull())
        return FString();

    switch (Value->Type)
    {
    case EJson::String:
        return Value->AsString();

    case EJson::Number:
        return FString::SanitizeFloat(Value->AsNumber());

    case EJson::Boolean:
        return Value->AsBool() ? TEXT("true") : TEXT("false");

    case EJson::Array:
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
        TArray<FString> Parts;
        Parts.Reserve(Arr.Num());
        for (const auto& Elem : Arr)
        {
            Parts.Add(JsonValueToAttrString(Elem));
        }
        return FString::Join(Parts, TEXT(","));
    }

    case EJson::Object:
    {
        // Serialize JSON object as compact key=value pairs for attribute embedding
        TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid()) return FString();
        TArray<FString> Fields;
        for (const auto& Pair : Obj->Values)
        {
            Fields.Add(EARGCompat::JsonKeyToString(Pair.Key) + TEXT("=") + JsonValueToAttrString(Pair.Value));
        }
        return TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
    }

    case EJson::Null:
    default:
        return FString();
    }
}

} // namespace WidgetXmlHelpers
