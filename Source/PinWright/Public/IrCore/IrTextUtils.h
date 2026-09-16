// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FProperty;
class FStructProperty;
class UObject;
class UStruct;

struct PINWRIGHT_API FReflectedFieldEmitOptions
{
    FString FieldSeparator = TEXT(": ");
    bool bEmitArraysAsBracketList = false;

    // Zero-default rule. AppendReflectedFields normally omits every property equal to
    // its archetype/CDO default, which makes an absent line ambiguous: the reader
    // cannot tell "at a default they would have to look up" from "false / 0 / unset",
    // and guesses the latter. That inverts on any property whose class default is not
    // the type's zero value (`UNiagaraSpriteRendererProperties::bSubImageBlend`
    // defaults to true; `SubImageSize` to (1,1)).
    //
    // With this set, a property sitting at a class default that is NOT its type's
    // zero value is emitted anyway, with a trailing ` @default` marker, and only
    // zero-valued defaults are omitted. Absence then carries exactly one meaning:
    // the property holds its type's zero value (false, 0, empty, null, identity).
    // Overridden properties are emitted unmarked, as before.
    //
    // Off by default. Adopting it in an IR whose text is also a compile input
    // requires that IR's parser to strip the ` @default` suffix first; NIR
    // (decompile-only) is the first adopter. See
    // B-nir-renderer-omits-default-valued-properties.
    bool bEmitNonZeroDefaults = false;
};

class PINWRIGHT_API FIrTextUtils
{
public:
    static bool IsUnescapedQuote(const FString& Str, int32 Index);
    static bool IsUnescapedBacktick(const FString& Str, int32 Index);
    static FString StripTrailingComment(const FString& Value);
    static TArray<int32> FindTopLevelDelimiterPositions(const FString& Str, TCHAR Delimiter, bool bStopAtFirst = false);
    static TArray<FString> SmartSplit(const FString& Str, TCHAR Delimiter);
    static int32 FindMatchingChar(const FString& Str, int32 OpenPos, TCHAR OpenChar, TCHAR CloseChar);
    static bool TryExtractPosition(FString& InOutLine, FVector2D& OutPosition, FString& OutError);

    static bool IsBareNameToken(const FString& Text);
    static bool TryUnwrapNameToken(const FString& Text, FString& OutName, FString& OutError);
    static FString FormatNameToken(const FString& Name);
    static FString CamelToSnakeIdentifier(const FString& Value, const FString& EmptyFallback = TEXT("node"));

    static bool TryUnwrapStringLiteral(const FString& Text, FString& OutValue, FString& OutError);

    // Strip an optional surrounding string-literal or name-token wrapper, returning
    // the bare payload; passes Text through unchanged when it carries neither wrapper.
    // The AGIR emitter quotes every struct/string/name/enum/path export, so reflective
    // ImportText callers must unwrap first (e.g. an empty struct exports as `"()"`,
    // and FProperty::ImportText for a struct rejects the leading `"`). Shared by the
    // AGIR compiler import sites and the anim-node reflective writer.
    static FString UnwrapStringOrNameToken(const FString& Text);

    static FString EscapeString(const FString& Value);
    static FString Quote(const FString& Value);

    static FString FormatPositionSuffix(int32 X, int32 Y);
    static FString FormatFieldList(const TArray<FString>& Fields);

    // Numeric local-id token shared by BPIR/MGIR/MSIR/CRIR decompilers: `n0`,
    // `n1`, ... The leading `%` reference sigil is added by callers at emission
    // time (e.g. CRIR's `%%s = unit ...` format), so this returns the bare
    // token only.
    static FString FormatNumericLocalId(int32 Counter);

    // Default is the matching archetype/CDO container (parallel to Container). It
    // is threaded into the struct-export fallback so a struct-valued property's
    // nested sub-fields suppress against their real per-field archetype defaults
    // instead of the property type's zero-value — otherwise a non-default
    // sub-field equal to its zero-value is silently dropped and a sub-field left
    // at a non-zero default is spuriously emitted. Pass nullptr for zero-value
    // suppression (leaf callers that do not diff against a default).
    static FString FormatReflectedPropertyValue(
        const void* Container,
        const void* Default,
        FProperty* Property,
        UObject* OwnerForExportText,
        const FReflectedFieldEmitOptions& Options = FReflectedFieldEmitOptions());

    static void AppendReflectedFields(
        UStruct* IteratedStruct,
        const void* Instance,
        const void* Default,
        UObject* OwnerForExportText,
        const TSet<FName>& ExplicitProperties,
        TFunctionRef<bool(const FStructProperty*)> RejectStruct,
        TFunctionRef<bool(FName)> RejectName,
        const FReflectedFieldEmitOptions& Options,
        TArray<FString>& OutFields);

    static bool IsSafeReflectedProperty(
        const FProperty* Property,
        TFunctionRef<bool(const FStructProperty*)> RejectStruct,
        TFunctionRef<bool(FName)> RejectName);
};
