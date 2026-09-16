// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "IrCore/IrTextUtils.h"

#include "Compat/EngineVersionCompat.h"

#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
// FStrProperty was split out of UObject/UnrealType.h into its own header in 5.5;
// on <5.5 the UnrealType.h include above provides the complete type.
#include "UObject/StrProperty.h"
#endif

namespace
{
bool IsEscapedAt(const FString& Str, int32 Index, TCHAR Marker)
{
    if (Index >= Str.Len() || Str[Index] != Marker)
    {
        return false;
    }

    int32 BackslashCount = 0;
    for (int32 Check = Index - 1; Check >= 0 && Str[Check] == TEXT('\\'); --Check)
    {
        ++BackslashCount;
    }

    return BackslashCount % 2 == 0;
}

TCHAR DecodeEscape(TCHAR Ch)
{
    switch (Ch)
    {
    case TEXT('n'): return TEXT('\n');
    case TEXT('r'): return TEXT('\r');
    case TEXT('t'): return TEXT('\t');
    default: return Ch;
    }
}

int32 HexDigitValue(TCHAR Ch)
{
    if (Ch >= TEXT('0') && Ch <= TEXT('9'))
    {
        return Ch - TEXT('0');
    }
    if (Ch >= TEXT('a') && Ch <= TEXT('f'))
    {
        return Ch - TEXT('a') + 10;
    }
    if (Ch >= TEXT('A') && Ch <= TEXT('F'))
    {
        return Ch - TEXT('A') + 10;
    }
    return INDEX_NONE;
}

bool DecodeDelimitedToken(const FString& Text, TCHAR Delimiter, FString& OutValue, FString& OutError)
{
    OutValue.Reset();
    OutError.Reset();

    const FString Trimmed = Text.TrimStartAndEnd();
    if (Trimmed.Len() < 2 || Trimmed[0] != Delimiter)
    {
        OutError = FString::Printf(TEXT("Expected token starting with '%c'."), Delimiter);
        return false;
    }

    bool bEscaped = false;
    for (int32 Index = 1; Index < Trimmed.Len(); ++Index)
    {
        const TCHAR Ch = Trimmed[Index];
        if (bEscaped)
        {
            if (Ch == TEXT('u'))
            {
                if (Index + 4 >= Trimmed.Len())
                {
                    OutError = TEXT("Incomplete unicode escape.");
                    return false;
                }

                int32 Codepoint = 0;
                for (int32 Offset = 1; Offset <= 4; ++Offset)
                {
                    const int32 Digit = HexDigitValue(Trimmed[Index + Offset]);
                    if (Digit == INDEX_NONE)
                    {
                        OutError = TEXT("Invalid unicode escape.");
                        return false;
                    }
                    Codepoint = (Codepoint << 4) | Digit;
                }
                OutValue.AppendChar(static_cast<TCHAR>(Codepoint));
                Index += 4;
                bEscaped = false;
                continue;
            }
            OutValue.AppendChar(DecodeEscape(Ch));
            bEscaped = false;
            continue;
        }

        if (Ch == TEXT('\\'))
        {
            bEscaped = true;
            continue;
        }

        if (Ch == Delimiter)
        {
            if (!Trimmed.Mid(Index + 1).TrimStartAndEnd().IsEmpty())
            {
                OutError = FString::Printf(TEXT("Unexpected text after closing '%c'."), Delimiter);
                return false;
            }
            return true;
        }

        OutValue.AppendChar(Ch);
    }

    OutError = FString::Printf(TEXT("Unterminated '%c' token."), Delimiter);
    return false;
}

// True when Value holds what a freshly default-constructed instance of the property's
// type would hold: false, 0, empty string / name / text / array, null object, identity
// struct. Drives the zero-default rule (FReflectedFieldEmitOptions::bEmitNonZeroDefaults) —
// omitting a property is only unambiguous when the omitted value is the one a reader
// guesses, which is the type's zero value and not necessarily the class default.
//
// FDefaultConstructedPropertyElement mallocs at the property's own alignment and runs
// InitializeValue (memzero for zero-constructible types, the struct's zero-then-construct
// otherwise), destroying on scope exit. Property->Identical(A, nullptr) is NOT a
// substitute: UScriptStruct::CompareScriptStruct answers "not identical" for a null
// comparand, so every struct property would read as non-zero.
bool IsAtTypeZeroValue(FProperty* Property, const void* Value)
{
    if (!Property || !Value)
    {
        return true;
    }

    const FDefaultConstructedPropertyElement ZeroValue(Property);
    const void* ZeroPtr = ZeroValue.GetObjAddress();
    return ZeroPtr != nullptr && Property->Identical(Value, ZeroPtr, PPF_None);
}

bool ShouldQuoteBracketArrayElement(const FProperty* Inner)
{
    return Inner && (Inner->IsA<FStrProperty>()
        || Inner->IsA<FNameProperty>()
        || Inner->IsA<FTextProperty>()
        || Inner->IsA<FObjectProperty>()
        || Inner->IsA<FClassProperty>()
        || Inner->IsA<FSoftObjectProperty>()
        || Inner->IsA<FSoftClassProperty>());
}
}

bool FIrTextUtils::IsUnescapedQuote(const FString& Str, int32 Index)
{
    return IsEscapedAt(Str, Index, TEXT('"'));
}

bool FIrTextUtils::IsUnescapedBacktick(const FString& Str, int32 Index)
{
    return IsEscapedAt(Str, Index, TEXT('`'));
}

FString FIrTextUtils::StripTrailingComment(const FString& Value)
{
    bool bInQuote = false;
    bool bInName = false;
    for (int32 Index = 0; Index < Value.Len(); ++Index)
    {
        const TCHAR Ch = Value[Index];
        if (!bInName && Ch == TEXT('"') && IsUnescapedQuote(Value, Index))
        {
            bInQuote = !bInQuote;
            continue;
        }
        if (!bInQuote && Ch == TEXT('`') && IsUnescapedBacktick(Value, Index))
        {
            bInName = !bInName;
            continue;
        }

        if (!bInQuote && !bInName && Ch == TEXT('#'))
        {
            return Value.Left(Index).TrimEnd();
        }
    }

    return Value;
}

TArray<int32> FIrTextUtils::FindTopLevelDelimiterPositions(const FString& Str, TCHAR Delimiter, bool bStopAtFirst)
{
    TArray<int32> Positions;
    int32 ParenDepth = 0;
    int32 AngleDepth = 0;
    int32 BracketDepth = 0;
    int32 BraceDepth = 0;
    bool bInQuote = false;
    bool bInName = false;

    for (int32 Index = 0; Index < Str.Len(); ++Index)
    {
        const TCHAR Ch = Str[Index];

        if (!bInName && Ch == TEXT('"') && IsUnescapedQuote(Str, Index))
        {
            bInQuote = !bInQuote;
            continue;
        }
        if (!bInQuote && Ch == TEXT('`') && IsUnescapedBacktick(Str, Index))
        {
            bInName = !bInName;
            continue;
        }
        if (bInQuote || bInName)
        {
            continue;
        }

        switch (Ch)
        {
        case TEXT('('): ++ParenDepth; break;
        case TEXT(')'): ParenDepth = FMath::Max(0, ParenDepth - 1); break;
        case TEXT('<'): ++AngleDepth; break;
        case TEXT('>'): AngleDepth = FMath::Max(0, AngleDepth - 1); break;
        case TEXT('['): ++BracketDepth; break;
        case TEXT(']'): BracketDepth = FMath::Max(0, BracketDepth - 1); break;
        case TEXT('{'): ++BraceDepth; break;
        case TEXT('}'): BraceDepth = FMath::Max(0, BraceDepth - 1); break;
        default: break;
        }

        if (Ch == Delimiter && ParenDepth == 0 && AngleDepth == 0 && BracketDepth == 0 && BraceDepth == 0)
        {
            Positions.Add(Index);
            if (bStopAtFirst)
            {
                break;
            }
        }
    }

    return Positions;
}

TArray<FString> FIrTextUtils::SmartSplit(const FString& Str, TCHAR Delimiter)
{
    TArray<FString> Parts;
    int32 Start = 0;

    for (const int32 DelimiterIndex : FindTopLevelDelimiterPositions(Str, Delimiter))
    {
        Parts.Add(Str.Mid(Start, DelimiterIndex - Start).TrimStartAndEnd());
        Start = DelimiterIndex + 1;
    }

    const FString Last = Str.Mid(Start).TrimStartAndEnd();
    if (!Last.IsEmpty())
    {
        Parts.Add(Last);
    }

    return Parts;
}

int32 FIrTextUtils::FindMatchingChar(const FString& Str, int32 OpenPos, TCHAR OpenChar, TCHAR CloseChar)
{
    if (OpenPos >= Str.Len() || Str[OpenPos] != OpenChar)
    {
        return INDEX_NONE;
    }

    int32 Depth = 1;
    bool bInQuote = false;
    bool bInName = false;
    for (int32 Index = OpenPos + 1; Index < Str.Len(); ++Index)
    {
        const TCHAR Ch = Str[Index];
        if (!bInName && Ch == TEXT('"') && IsUnescapedQuote(Str, Index))
        {
            bInQuote = !bInQuote;
            continue;
        }
        if (!bInQuote && Ch == TEXT('`') && IsUnescapedBacktick(Str, Index))
        {
            bInName = !bInName;
            continue;
        }
        if (bInQuote || bInName)
        {
            continue;
        }

        if (Ch == OpenChar)
        {
            ++Depth;
        }
        else if (Ch == CloseChar)
        {
            --Depth;
            if (Depth == 0)
            {
                return Index;
            }
        }
    }

    return INDEX_NONE;
}

bool FIrTextUtils::TryExtractPosition(FString& InOutLine, FVector2D& OutPosition, FString& OutError)
{
    OutError.Reset();
    bool bInQuote = false;
    bool bInName = false;
    int32 MarkerPos = INDEX_NONE;

    for (int32 Index = 0; Index < InOutLine.Len() - 1; ++Index)
    {
        if (!bInName && InOutLine[Index] == TEXT('"') && IsUnescapedQuote(InOutLine, Index))
        {
            bInQuote = !bInQuote;
            continue;
        }
        if (!bInQuote && InOutLine[Index] == TEXT('`') && IsUnescapedBacktick(InOutLine, Index))
        {
            bInName = !bInName;
            continue;
        }
        if (!bInQuote && !bInName && InOutLine[Index] == TEXT('@') && InOutLine[Index + 1] == TEXT('('))
        {
            MarkerPos = Index;
        }
    }

    if (MarkerPos == INDEX_NONE)
    {
        return false;
    }

    const int32 OpenParen = MarkerPos + 1;
    const int32 CloseParen = FindMatchingChar(InOutLine, OpenParen, TEXT('('), TEXT(')'));
    if (CloseParen == INDEX_NONE)
    {
        OutError = TEXT("Unmatched '@(' position marker");
        return false;
    }

    const FString Contents = InOutLine.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
    const TArray<FString> Parts = SmartSplit(Contents, TEXT(','));
    if (Parts.Num() != 2)
    {
        OutError = TEXT("Position marker requires '@(x, y)'");
        return false;
    }

    const FString XText = Parts[0].TrimStartAndEnd();
    const FString YText = Parts[1].TrimStartAndEnd();
    if (!LexTryParseString(OutPosition.X, *XText) ||
        !LexTryParseString(OutPosition.Y, *YText))
    {
        OutError = TEXT("Position marker coordinates must be numeric");
        return false;
    }

    InOutLine = (InOutLine.Left(MarkerPos) + InOutLine.Mid(CloseParen + 1)).TrimStartAndEnd();
    return true;
}

bool FIrTextUtils::IsBareNameToken(const FString& Text)
{
    if (Text.IsEmpty() || !(FChar::IsAlpha(Text[0]) || Text[0] == TEXT('_')))
    {
        return false;
    }

    for (int32 Index = 1; Index < Text.Len(); ++Index)
    {
        const TCHAR Ch = Text[Index];
        if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
        {
            return false;
        }
    }

    return true;
}

bool FIrTextUtils::TryUnwrapNameToken(const FString& Text, FString& OutName, FString& OutError)
{
    OutName.Reset();
    OutError.Reset();

    const FString Trimmed = Text.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        OutError = TEXT("Name token is empty.");
        return false;
    }

    if (Trimmed[0] == TEXT('"'))
    {
        OutError = TEXT("Double-quoted strings are not valid name tokens; use backticks for names.");
        return false;
    }

    if (Trimmed[0] == TEXT('`'))
    {
        return DecodeDelimitedToken(Trimmed, TEXT('`'), OutName, OutError);
    }

    if (!IsBareNameToken(Trimmed))
    {
        OutError = TEXT("Name token must be a bare identifier or a backtick-delimited name.");
        return false;
    }

    OutName = Trimmed;
    return true;
}

FString FIrTextUtils::FormatNameToken(const FString& Name)
{
    if (IsBareNameToken(Name))
    {
        return Name;
    }

    FString Escaped;
    Escaped.Reserve(Name.Len());
    for (int32 Index = 0; Index < Name.Len(); ++Index)
    {
        const TCHAR Ch = Name[Index];
        switch (Ch)
        {
        case TEXT('\\'):
        case TEXT('`'):
            Escaped.AppendChar(TEXT('\\'));
            Escaped.AppendChar(Ch);
            break;
        case TEXT('\n'):
            Escaped += TEXT("\\n");
            break;
        case TEXT('\r'):
            Escaped += TEXT("\\r");
            break;
        case TEXT('\t'):
            Escaped += TEXT("\\t");
            break;
        default:
            if (Ch < 0x20)
            {
                Escaped += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Ch));
            }
            else
            {
                Escaped.AppendChar(Ch);
            }
            break;
        }
    }

    return FString::Printf(TEXT("`%s`"), *Escaped);
}

FString FIrTextUtils::CamelToSnakeIdentifier(const FString& Value, const FString& EmptyFallback)
{
    FString Result;
    Result.Reserve(Value.Len() + 4);
    for (int32 Index = 0; Index < Value.Len(); ++Index)
    {
        const TCHAR Ch = Value[Index];
        if (Ch == TEXT('_') || Ch == TEXT(' ') || Ch == TEXT('-'))
        {
            if (!Result.IsEmpty() && !Result.EndsWith(TEXT("_")))
            {
                Result.AppendChar(TEXT('_'));
            }
            continue;
        }

        if (FChar::IsUpper(Ch))
        {
            const bool bPreviousIsLowerOrDigit =
                Index > 0 && (FChar::IsLower(Value[Index - 1]) || FChar::IsDigit(Value[Index - 1]));
            const bool bNextIsLower =
                Index + 1 < Value.Len() && FChar::IsLower(Value[Index + 1]);
            if (!Result.IsEmpty() && !Result.EndsWith(TEXT("_")) && (bPreviousIsLowerOrDigit || bNextIsLower))
            {
                Result.AppendChar(TEXT('_'));
            }
        }

        Result.AppendChar(FChar::ToLower(Ch));
    }

    while (Result.EndsWith(TEXT("_")))
    {
        Result.LeftChopInline(1);
    }
    return Result.IsEmpty() ? EmptyFallback : Result;
}

bool FIrTextUtils::TryUnwrapStringLiteral(const FString& Text, FString& OutValue, FString& OutError)
{
    return DecodeDelimitedToken(Text, TEXT('"'), OutValue, OutError);
}

FString FIrTextUtils::UnwrapStringOrNameToken(const FString& Text)
{
    FString Unwrapped;
    FString Error;
    if (TryUnwrapStringLiteral(Text, Unwrapped, Error)
        || TryUnwrapNameToken(Text, Unwrapped, Error))
    {
        return Unwrapped;
    }
    return Text;
}

FString FIrTextUtils::EscapeString(const FString& Value)
{
    FString Result;
    Result.Reserve(Value.Len());
    for (int32 Index = 0; Index < Value.Len(); ++Index)
    {
        const TCHAR Ch = Value[Index];
        switch (Ch)
        {
        case TEXT('\\'):
            Result += TEXT("\\\\");
            break;
        case TEXT('"'):
            Result += TEXT("\\\"");
            break;
        case TEXT('\n'):
            Result += TEXT("\\n");
            break;
        case TEXT('\r'):
            Result += TEXT("\\r");
            break;
        case TEXT('\t'):
            Result += TEXT("\\t");
            break;
        default:
            if (Ch < 0x20)
            {
                Result += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Ch));
            }
            else
            {
                Result.AppendChar(Ch);
            }
            break;
        }
    }

    return Result;
}

FString FIrTextUtils::Quote(const FString& Value)
{
    return FString::Printf(TEXT("\"%s\""), *EscapeString(Value));
}

FString FIrTextUtils::FormatPositionSuffix(int32 X, int32 Y)
{
    return FString::Printf(TEXT(" @(%d, %d)"), X, Y);
}

FString FIrTextUtils::FormatNumericLocalId(int32 Counter)
{
    return FString::Printf(TEXT("n%d"), Counter);
}

FString FIrTextUtils::FormatFieldList(const TArray<FString>& Fields)
{
    if (Fields.Num() == 0)
    {
        return TEXT("()");
    }

    return TEXT("(") + FString::Join(Fields, TEXT(", ")) + TEXT(")");
}

FString FIrTextUtils::FormatReflectedPropertyValue(
    const void* Container,
    const void* Default,
    FProperty* Property,
    UObject* OwnerForExportText,
    const FReflectedFieldEmitOptions& Options)
{
    if (!Container || !Property)
    {
        return Quote(FString());
    }

    if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
    {
        const void* ValuePtr = BoolProperty->ContainerPtrToValuePtr<void>(Container);
        return BoolProperty->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
    }

    if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
        NumericProperty && !Property->IsA<FByteProperty>())
    {
        const void* ValuePtr = NumericProperty->ContainerPtrToValuePtr<void>(Container);
        if (NumericProperty->IsFloatingPoint())
        {
            return FString::SanitizeFloat(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
        }

        if (NumericProperty->IsInteger())
        {
            return FString::Printf(TEXT("%lld"), NumericProperty->GetSignedIntPropertyValue(ValuePtr));
        }
    }

    if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        const void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(Container);
        const int64 Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
        return Quote(EnumProperty->GetEnum()->GetNameStringByValue(Value));
    }

    if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        const void* ValuePtr = ByteProperty->ContainerPtrToValuePtr<void>(Container);
        if (ByteProperty->Enum)
        {
            return Quote(ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetSignedIntPropertyValue(ValuePtr)));
        }

        return FString::Printf(TEXT("%lld"), ByteProperty->GetSignedIntPropertyValue(ValuePtr));
    }

    if (FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
    {
        UObject* Value = ClassProperty->GetObjectPropertyValue(ClassProperty->ContainerPtrToValuePtr<void>(Container));
        return Quote(Value ? Value->GetPathName() : FString());
    }

    if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
    {
        UObject* Value = ObjectProperty->GetObjectPropertyValue(ObjectProperty->ContainerPtrToValuePtr<void>(Container));
        return Quote(Value ? Value->GetPathName() : FString());
    }

    if (FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
    {
        const FSoftObjectPtr* Value = SoftClassProperty->ContainerPtrToValuePtr<FSoftObjectPtr>(Container);
        return Quote(Value ? Value->ToSoftObjectPath().ToString() : FString());
    }

    if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
    {
        const FSoftObjectPtr* Value = SoftObjectProperty->ContainerPtrToValuePtr<FSoftObjectPtr>(Container);
        return Quote(Value ? Value->ToSoftObjectPath().ToString() : FString());
    }

    if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
        ArrayProperty && Options.bEmitArraysAsBracketList)
    {
        FScriptArrayHelper Helper(
            ArrayProperty,
            ArrayProperty->ContainerPtrToValuePtr<void>(Container));
        TArray<FString> ElementStrings;
        ElementStrings.Reserve(Helper.Num());

        FProperty* Inner = ArrayProperty->Inner;
        for (int32 ElementIndex = 0; ElementIndex < Helper.Num(); ++ElementIndex)
        {
            const void* ElementPtr = Helper.GetRawPtr(ElementIndex);
            FString Exported;
            if (Inner)
            {
                // Bracket-list elements export against a nullptr default: an array has
                // no per-element archetype to diff against, so each element renders at
                // full value. Unlike the scalar FStructProperty fallback below, struct-
                // typed elements are intentionally NOT diffed against per-field CDO
                // defaults here — the nullptr is deliberate, not the sub-field-drop bug.
                Inner->ExportTextItem_Direct(Exported, ElementPtr, nullptr, OwnerForExportText, PPF_None);
            }

            ElementStrings.Add(ShouldQuoteBracketArrayElement(Inner) ? Quote(Exported) : Exported);
        }

        return FString::Printf(TEXT("[%s]"), *FString::Join(ElementStrings, TEXT(",")));
    }

    // Pass the real archetype default (not nullptr): ExportText then diffs each
    // FStructProperty sub-field against its per-field default instead of collapsing
    // to Identical(value, 0). See this function's header doc comment for why.
    //
    // Both pointers must be VALUE pointers (offset to this property's field). Use
    // ExportTextItem_Direct with ContainerPtrToValuePtr on each — NOT
    // ExportTextItem_InContainer(Container, Default): that helper offset-adjusts
    // only the value, passing DefaultValue through raw (see FStructProperty::
    // ExportText_Internal, which forwards DefaultValue unadjusted to
    // UScriptStruct::ExportText). Handing it the container-level Default there
    // reads the default at the wrong offset (the CDO base, not &cdo.Field), so
    // every sub-field mis-compares and defaulted sub-fields are spuriously emitted.
    FString Exported;
    const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
    const void* DefaultValuePtr =
        Default ? Property->ContainerPtrToValuePtr<void>(Default) : nullptr;
    Property->ExportTextItem_Direct(Exported, ValuePtr, DefaultValuePtr, OwnerForExportText, PPF_None);
    return Quote(Exported);
}

void FIrTextUtils::AppendReflectedFields(
    UStruct* IteratedStruct,
    const void* Instance,
    const void* Default,
    UObject* OwnerForExportText,
    const TSet<FName>& ExplicitProperties,
    TFunctionRef<bool(const FStructProperty*)> RejectStruct,
    TFunctionRef<bool(FName)> RejectName,
    const FReflectedFieldEmitOptions& Options,
    TArray<FString>& OutFields)
{
    if (!IteratedStruct || !Instance)
    {
        return;
    }

    TArray<FProperty*> Properties;
    for (TFieldIterator<FProperty> It(IteratedStruct, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        FProperty* Property = *It;
        if (Property
            && !ExplicitProperties.Contains(Property->GetFName())
            && IsSafeReflectedProperty(Property, RejectStruct, RejectName))
        {
            Properties.Add(Property);
        }
    }

    Properties.Sort([](const FProperty& A, const FProperty& B)
    {
        return A.GetName() < B.GetName();
    });

    for (FProperty* Property : Properties)
    {
        const void* InstanceValue = Property->ContainerPtrToValuePtr<void>(Instance);
        const bool bAtClassDefault = Default != nullptr
            && Property->Identical(
                InstanceValue,
                Property->ContainerPtrToValuePtr<void>(Default),
                PPF_None);

        if (bAtClassDefault
            && (!Options.bEmitNonZeroDefaults || IsAtTypeZeroValue(Property, InstanceValue)))
        {
            continue;
        }

        // A property emitted only because it sits at a non-zero class default has an
        // empty diff against that default (a struct would export as "()"), so format it
        // against a nullptr default to print the whole resolved value. Overridden
        // properties keep the archetype diff: their struct sub-fields must still
        // suppress against per-field defaults (B-decompile-struct-subfield-dropped).
        const FString Value = FormatReflectedPropertyValue(
            Instance,
            bAtClassDefault ? nullptr : Default,
            Property,
            OwnerForExportText,
            Options);
        OutFields.Add(FString::Printf(
            TEXT("%s%s%s%s"),
            *Property->GetName(),
            *Options.FieldSeparator,
            *Value,
            bAtClassDefault ? TEXT(" @default") : TEXT("")));
    }
}

bool FIrTextUtils::IsSafeReflectedProperty(
    const FProperty* Property,
    TFunctionRef<bool(const FStructProperty*)> RejectStruct,
    TFunctionRef<bool(FName)> RejectName)
{
    if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
    {
        return false;
    }

    static const EPropertyFlags UnsafeFlags = static_cast<EPropertyFlags>(
        CPF_Transient
        | CPF_DuplicateTransient
        | CPF_NonPIEDuplicateTransient
        | CPF_Deprecated
        | CPF_SkipSerialization
        | CPF_TextExportTransient);
    if (Property->HasAnyPropertyFlags(UnsafeFlags) || RejectName(Property->GetFName()))
    {
        return false;
    }

    if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
    {
        if (RejectStruct(StructProp))
        {
            return false;
        }
    }

    return Property->IsA<FBoolProperty>()
        || Property->IsA<FNumericProperty>()
        || Property->IsA<FEnumProperty>()
        || Property->IsA<FByteProperty>()
        || Property->IsA<FStrProperty>()
        || Property->IsA<FNameProperty>()
        || Property->IsA<FTextProperty>()
        || Property->IsA<FObjectProperty>()
        || Property->IsA<FClassProperty>()
        || Property->IsA<FSoftObjectProperty>()
        || Property->IsA<FSoftClassProperty>()
        || Property->IsA<FStructProperty>()
        || Property->IsA<FArrayProperty>();
}
