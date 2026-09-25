// Copyright (c) 2026 Alexander Penkin. MIT License.

// JSON / text to property assignment utilities for PinWright
#include "Utils/PropertyImport.h"

#include "Utils/JsonUtils.h"
#include "Utils/PropertyInspection.h"

#include "Compat/EngineVersionCompat.h"
#include "Engine/Blueprint.h"
#include "Internationalization/Text.h"
#include "Utils/GuardedLoad.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "Misc/PackageName.h"

namespace
{
    bool HasLocalizationIdentity(const FText& Text, FString* OutNamespace = nullptr, FString* OutKey = nullptr)
    {
        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Text);
        const TOptional<FString> Key = FTextInspector::GetKey(Text);
        if (!Namespace.IsSet() || Namespace.GetValue().IsEmpty() || !Key.IsSet() || Key.GetValue().IsEmpty())
        {
            return false;
        }
        if (OutNamespace)
        {
            *OutNamespace = Namespace.GetValue();
        }
        if (OutKey)
        {
            *OutKey = Key.GetValue();
        }
        return true;
    }

    // Sanity-check a candidate UObject-path string before handing it to
    // StaticLoadObject / LoadObject. Those eventually call FName::FName(*Path)
    // which asserts (abort, not recoverable) if the input exceeds NAME_SIZE-1
    // (1023) characters, or if the input is a struct-text blob the importer
    // mis-routed here. Rejecting up front keeps a malformed XML attribute from
    // crashing the editor — the caller surfaces a normal property-apply error.
    //
    // We treat as "not a real path" anything that:
    //   - is too long for FName to swallow, OR
    //   - contains characters that never appear in valid /Package.Asset paths:
    //     '{' '}' '=' ',' '\n' '\r' '\t' ' ' (a real path has no whitespace or
    //     struct-text punctuation; ':' inside SubobjectPath segments is fine).
    bool IsPlausibleObjectPathForLoad(const FString& Path)
    {
        if (Path.Len() >= NAME_SIZE)
        {
            return false;
        }
        for (TCHAR Ch : Path)
        {
            switch (Ch)
            {
            case TEXT('{'):
            case TEXT('}'):
            case TEXT('='):
            case TEXT(','):
            case TEXT('\n'):
            case TEXT('\r'):
            case TEXT('\t'):
            case TEXT(' '):
                return false;
            default:
                break;
            }
        }
        return true;
    }

    // True when a string value passed to an object/class/soft-ref property is the
    // canonical "clear to null" sentinel rather than an asset path to load. UE
    // prints a null UObject* as "None", and reflection round-trips treat
    // {"", "None", "null"} as the empty reference — the decompiler's
    // IsPinOmittableAtCallSite already short-circuits object-ref pins on
    // DefaultValue in {"", "None"}. Without this, "None" reaches LoadObject and
    // produces a misleading "Failed to load object at path: None", and the
    // soft-ref branches silently store a literal FSoftObjectPath("None").
    // Matched case-insensitively after trimming whitespace.
    bool IsNullObjectSentinel(const FString& Value)
    {
        const FString Trimmed = Value.TrimStartAndEnd();
        return Trimmed.IsEmpty()
            || Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase)
            || Trimmed.Equals(TEXT("null"), ESearchCase::IgnoreCase);
    }

    // Resolves a TSoftClassPtr value to the UClass it names, on any mount point: a class path
    // (/Script/Engine.Actor, /Root/Dir/BP_X.BP_X_C), a Blueprint object path (/Root/Dir/BP_X.BP_X)
    // or a bare Blueprint package path (/Root/Dir/BP_X); a Blueprint yields its GeneratedClass.
    // The object is found or loaded rather than guessed from a path prefix (the old rule appended
    // _C only under /Game/ and sat behind the FSoftObjectProperty branch, so every soft-class
    // value was stored verbatim, unresolvable or not, and reported as success -
    // B-set-default-softclass-missing-c-suffix). Null + OutError on a miss.
    UClass* ResolveSoftClassValue(const FString& Path, FString& OutError)
    {
        if (!IsPlausibleObjectPathForLoad(Path))
        {
            OutError = FString::Printf(
                TEXT("Soft class property value is not a valid path (length=%d, contains struct-text punctuation or exceeds FName limit)"),
                Path.Len());
            return nullptr;
        }
        const FString ObjectPath = Path.Contains(TEXT("."))
            ? Path
            : FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
        FString Refusal;
        UObject* Obj = PinWrightGuardedLoad::LoadObjectChecked<UObject>(
            ObjectPath, &Refusal, LOAD_NoWarn | LOAD_Quiet);
        UClass* Class = Cast<UClass>(Obj);
        if (const UBlueprint* Blueprint = Cast<UBlueprint>(Obj))
        {
            Class = Blueprint->GeneratedClass;
        }
        if (!Class)
        {
            OutError = !Refusal.IsEmpty() ? Refusal
                : Obj ? FString::Printf(TEXT("Soft class value '%s' names a %s, not a class or Blueprint"),
                        *Path, *Obj->GetClass()->GetName())
                : FString::Printf(TEXT("Failed to resolve class at path: %s"), *Path);
        }
        return Class;
    }

    bool IsJsonScalarPropertyImpl(const FProperty* Property)
    {
        return Property
            && (CastField<FBoolProperty>(Property)
                || CastField<FNumericProperty>(Property)
                || CastField<FEnumProperty>(Property));
    }

    bool ApplyJsonValueToPropertyDirect(void* TargetValue, FProperty* Property,
                                        const TSharedPtr<FJsonValue>& ValueField,
                                        FString& OutError)
    {
        OutError.Empty();
        if (!TargetValue || !Property || !ValueField)
        {
            OutError = TEXT("Invalid target/property/value");
            return false;
        }

        if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            bool bValue = false;
            if (!TryParseStrictJsonBoolean(ValueField, bValue, OutError))
            {
                return false;
            }
            BoolProperty->SetPropertyValue(TargetValue, bValue);
            return true;
        }

        if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            if (UEnum* Enum = ByteProperty->Enum)
            {
                if (ValueField->Type == EJson::String)
                {
                    const FString InString = ValueField->AsString();
                    int64 EnumValue = Enum->GetValueByNameString(InString);
                    if (EnumValue == INDEX_NONE)
                    {
                        const FString FullName = Enum->GenerateFullEnumName(*InString);
                        EnumValue = Enum->GetValueByName(FName(*FullName));
                    }
                    if (EnumValue == INDEX_NONE)
                    {
                        OutError = FString::Printf(TEXT("Invalid enum value '%s' for enum '%s'"),
                                                   *InString, *Enum->GetName());
                        return false;
                    }
                    ByteProperty->SetPropertyValue(TargetValue, static_cast<uint8>(EnumValue));
                    return true;
                }

                int64 EnumValue = 0;
                if (!TryParseStrictJsonInteger(
                        ValueField, TNumericLimits<int64>::Min(),
                        TNumericLimits<int64>::Max(), EnumValue, OutError))
                {
                    return false;
                }
                if (!Enum->IsValidEnumValue(EnumValue))
                {
                    OutError = FString::Printf(
                        TEXT("Numeric value %lld is not valid for enum '%s'"), EnumValue,
                        *Enum->GetName());
                    return false;
                }
                ByteProperty->SetPropertyValue(TargetValue, static_cast<uint8>(EnumValue));
                return true;
            }

            uint64 Value = 0;
            if (!TryParseStrictJsonUnsignedInteger(
                    ValueField, TNumericLimits<uint8>::Max(), Value, OutError))
            {
                return false;
            }
            ByteProperty->SetPropertyValue(TargetValue, static_cast<uint8>(Value));
            return true;
        }

        if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            UEnum* Enum = EnumProperty->GetEnum();
            FNumericProperty* UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
            if (!Enum || !UnderlyingProperty)
            {
                OutError = TEXT("Enum property has no valid enum definition");
                return false;
            }
            if (ValueField->Type == EJson::String)
            {
                const FString InString = ValueField->AsString();
                int64 EnumValue = Enum->GetValueByNameString(InString);
                if (EnumValue == INDEX_NONE)
                {
                    const FString FullName = Enum->GenerateFullEnumName(*InString);
                    EnumValue = Enum->GetValueByName(FName(*FullName));
                }
                if (EnumValue == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("Invalid enum value '%s' for enum '%s'"),
                                               *InString, *Enum->GetName());
                    return false;
                }
                UnderlyingProperty->SetIntPropertyValue(TargetValue, EnumValue);
                return true;
            }

            int64 EnumValue = 0;
            if (!TryParseStrictJsonInteger(
                    ValueField, TNumericLimits<int64>::Min(),
                    TNumericLimits<int64>::Max(), EnumValue, OutError))
            {
                return false;
            }
            if (!Enum->IsValidEnumValue(EnumValue))
            {
                OutError = FString::Printf(
                    TEXT("Numeric value %lld is not valid for enum '%s'"), EnumValue,
                    *Enum->GetName());
                return false;
            }
            UnderlyingProperty->SetIntPropertyValue(TargetValue, EnumValue);
            return true;
        }

        if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
        {
            if (NumericProperty->IsFloatingPoint())
            {
                double Value = 0.0;
                if (!TryParseStrictJsonNumber(ValueField, Value, OutError))
                {
                    return false;
                }
                if (MCP_PROPERTY_ELEMENT_SIZE(NumericProperty) == sizeof(float))
                {
                    const float FloatValue = static_cast<float>(Value);
                    if (!FMath::IsFinite(FloatValue))
                    {
                        OutError = TEXT("Float value is outside the finite float range");
                        return false;
                    }
                }
                NumericProperty->SetFloatingPointPropertyValue(TargetValue, Value);
                return true;
            }

            if (FInt8Property* Int8Property = CastField<FInt8Property>(Property))
            {
                int64 Value = 0;
                if (!TryParseStrictJsonInteger(
                        ValueField, TNumericLimits<int8>::Min(),
                        TNumericLimits<int8>::Max(), Value, OutError))
                {
                    return false;
                }
                Int8Property->SetIntPropertyValue(TargetValue, Value);
                return true;
            }
            if (FInt16Property* Int16Property = CastField<FInt16Property>(Property))
            {
                int64 Value = 0;
                if (!TryParseStrictJsonInteger(
                        ValueField, TNumericLimits<int16>::Min(),
                        TNumericLimits<int16>::Max(), Value, OutError))
                {
                    return false;
                }
                Int16Property->SetIntPropertyValue(TargetValue, Value);
                return true;
            }
            if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
            {
                int64 Value = 0;
                if (!TryParseStrictJsonInteger(
                        ValueField, TNumericLimits<int32>::Min(),
                        TNumericLimits<int32>::Max(), Value, OutError))
                {
                    return false;
                }
                IntProperty->SetIntPropertyValue(TargetValue, Value);
                return true;
            }
            if (FInt64Property* Int64Property = CastField<FInt64Property>(Property))
            {
                int64 Value = 0;
                if (!TryParseStrictJsonInteger(
                        ValueField, TNumericLimits<int64>::Min(),
                        TNumericLimits<int64>::Max(), Value, OutError))
                {
                    return false;
                }
                Int64Property->SetIntPropertyValue(TargetValue, Value);
                return true;
            }
            if (FUInt16Property* UInt16Property = CastField<FUInt16Property>(Property))
            {
                uint64 Value = 0;
                if (!TryParseStrictJsonUnsignedInteger(
                        ValueField, TNumericLimits<uint16>::Max(), Value, OutError))
                {
                    return false;
                }
                UInt16Property->SetIntPropertyValue(TargetValue, Value);
                return true;
            }
            if (FUInt32Property* UInt32Property = CastField<FUInt32Property>(Property))
            {
                uint64 Value = 0;
                if (!TryParseStrictJsonUnsignedInteger(
                        ValueField, TNumericLimits<uint32>::Max(), Value, OutError))
                {
                    return false;
                }
                UInt32Property->SetIntPropertyValue(TargetValue, Value);
                return true;
            }
            if (FUInt64Property* UInt64Property = CastField<FUInt64Property>(Property))
            {
                uint64 Value = 0;
                if (!TryParseStrictJsonUnsignedInteger(
                        ValueField, TNumericLimits<uint64>::Max(), Value, OutError))
                {
                    return false;
                }
                UInt64Property->SetIntPropertyValue(TargetValue, Value);
                return true;
            }
        }

        OutError = TEXT("Unsupported reflected scalar property type");
        return false;
    }
}

// Per-property-type coercion shared by widget XML import and BPIR node_props
// replay. Scalar strings intentionally remain strings so ApplyJsonValueToProperty
// can validate the original spelling before committing it; converting malformed
// text with Atod/Atoi or a bool equality check would turn it into zero/false.
// Structured/domain-specific forms below retain their existing shape coercions.
TSharedPtr<FJsonValue> CoerceStringToJsonValueByProperty(const FString& StringValue, FProperty* TargetProperty)
{
    if (!TargetProperty)
    {
        return MakeShared<FJsonValueString>(StringValue);
    }

    if (CastField<FBoolProperty>(TargetProperty)
        || CastField<FFloatProperty>(TargetProperty)
        || CastField<FDoubleProperty>(TargetProperty)
        || CastField<FIntProperty>(TargetProperty)
        || CastField<FInt64Property>(TargetProperty)
        || CastField<FUInt16Property>(TargetProperty)
        || CastField<FUInt32Property>(TargetProperty)
        || CastField<FUInt64Property>(TargetProperty)
        || CastField<FByteProperty>(TargetProperty)
        || CastField<FEnumProperty>(TargetProperty))
    {
        return MakeShared<FJsonValueString>(StringValue);
    }

    if (CastField<FStrProperty>(TargetProperty)
        || CastField<FNameProperty>(TargetProperty)
        || CastField<FTextProperty>(TargetProperty))
    {
        return MakeShared<FJsonValueString>(StringValue);
    }

    if (FStructProperty* StructProp = CastField<FStructProperty>(TargetProperty))
    {
        const FString StructName = StructProp->Struct->GetName();
        // Vector / Rotator XML attribute form: comma-split into 3 numbers
        if (StructName == TEXT("Vector") || StructName == TEXT("Rotator"))
        {
            TArray<FString> Parts;
            StringValue.ParseIntoArray(Parts, TEXT(","));
            if (Parts.Num() == 3)
            {
                TArray<TSharedPtr<FJsonValue>> NumArr;
                NumArr.Reserve(3);
                bool bAllValid = true;
                for (const FString& P : Parts)
                {
                    const FString Trimmed = P.TrimStartAndEnd();
                    if (Trimmed.IsEmpty())
                    {
                        bAllValid = false;
                        break;
                    }
                    NumArr.Add(MakeShared<FJsonValueNumber>(FCString::Atod(*Trimmed)));
                }
                if (bAllValid)
                {
                    return MakeShared<FJsonValueArray>(NumArr);
                }
            }
            return MakeShared<FJsonValueString>(StringValue);
        }
        // Embedded JSON object form (BPIR node_props struct values arrive this way);
        // ExportText format ("(R=1,G=0,...)") falls through to string and is handled
        // by ImportTextToProperty downstream.
        const FString Trimmed = StringValue.TrimStartAndEnd();
        if (Trimmed.StartsWith(TEXT("{")))
        {
            TSharedPtr<FJsonObject> Obj;
            const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Trimmed);
            if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
            {
                return MakeShared<FJsonValueObject>(Obj);
            }
            // Not valid JSON: this is the widget-XML exporter's brace/`=` struct
            // hybrid `{Key=Value,...}` (JsonValueToAttrString, EJson::Object case in
            // Handlers/UI/WidgetXmlUtils.h). Rewriting the structural braces to an
            // ExportText struct literal `(Key=Value,...)` lets ImportText_Direct
            // consume it downstream, so an exported widget tree round-trips back
            // through import. A genuine JSON object already returned above, and a paren
            // literal never entered this branch, so only the exporter's unparseable
            // hybrid reaches here.
            //
            // KNOWN LIMITATION: this is a blind character swap, not a grammar-aware
            // one. The supported input is a struct of scalar / nested-struct leaves,
            // whose only braces are the structural delimiters. It does NOT round-trip
            // two exporter forms because they are ambiguous once flattened: (1) a
            // string/text leaf that itself contains a brace — the exporter emits leaf
            // strings verbatim and unquoted (EJson::String case), so an FText holding
            // a `{0}` format placeholder is swapped to `(0)`; and (2) an array-valued
            // field — the exporter joins array elements with bare commas and no
            // brackets (EJson::Array case), which ImportText_Direct cannot reconstruct.
            FString AsExportTextLiteral = Trimmed;
            for (TCHAR& Ch : AsExportTextLiteral)
            {
                if (Ch == TEXT('{')) { Ch = TEXT('('); }
                else if (Ch == TEXT('}')) { Ch = TEXT(')'); }
            }
            return MakeShared<FJsonValueString>(AsExportTextLiteral);
        }
        return MakeShared<FJsonValueString>(StringValue);
    }

    if (CastField<FObjectProperty>(TargetProperty)
        || CastField<FSoftObjectProperty>(TargetProperty)
        || CastField<FSoftClassProperty>(TargetProperty))
    {
        return MakeShared<FJsonValueString>(StringValue);
    }

    if (CastField<FArrayProperty>(TargetProperty))
    {
        if (StringValue.StartsWith(TEXT("[")))
        {
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StringValue);
            TArray<TSharedPtr<FJsonValue>> ParsedArr;
            if (FJsonSerializer::Deserialize(Reader, ParsedArr))
            {
                return MakeShared<FJsonValueArray>(ParsedArr);
            }
        }
        TArray<FString> Parts;
        StringValue.ParseIntoArray(Parts, TEXT(","));
        TArray<TSharedPtr<FJsonValue>> StrArr;
        StrArr.Reserve(Parts.Num());
        for (const FString& P : Parts)
        {
            StrArr.Add(MakeShared<FJsonValueString>(P.TrimStartAndEnd()));
        }
        return MakeShared<FJsonValueArray>(StrArr);
    }

    return MakeShared<FJsonValueString>(StringValue);
}

bool IsJsonScalarProperty(const FProperty* Property)
{
    return IsJsonScalarPropertyImpl(Property);
}

bool TryStageJsonValueForProperty(FProperty* Property,
                                  const TSharedPtr<FJsonValue>& ValueField,
                                  void*& OutStagedValue,
                                  FString& OutError)
{
    OutStagedValue = nullptr;
    OutError.Empty();
    if (!IsJsonScalarProperty(Property))
    {
        OutError = TEXT("Property is not a supported reflected scalar");
        return false;
    }

    OutStagedValue = Property->AllocateAndInitializeValue();
    if (!OutStagedValue)
    {
        OutError = FString::Printf(TEXT("Failed to allocate scratch storage for property '%s'"),
                                   *Property->GetName());
        return false;
    }
    if (ApplyJsonValueToPropertyDirect(OutStagedValue, Property, ValueField, OutError))
    {
        return true;
    }

    Property->DestroyValue(OutStagedValue);
    FMemory::Free(OutStagedValue);
    OutStagedValue = nullptr;
    return false;
}

bool CoerceStringToPersistedFText(
    const FString& TextValue,
    const FText* ExistingText,
    FText& OutText,
    FString& OutError)
{
    OutError.Empty();

    FText ParsedText = FTextStringHelper::CreateFromBuffer(*TextValue);
    if (HasLocalizationIdentity(ParsedText))
    {
        OutText = MoveTemp(ParsedText);
        return true;
    }

    // String-table-backed FText carries identity via FTextHistory_StringTableEntry,
    // not via Namespace/Key. FTextInspector::GetNamespace/GetKey return empty when
    // the referenced table asset isn't loaded yet at compile time, so
    // HasLocalizationIdentity reports false.
    if (ParsedText.IsFromStringTable())
    {
        OutText = MoveTemp(ParsedText);
        return true;
    }

    FString ExistingNamespace;
    FString ExistingKey;
    if (ExistingText && HasLocalizationIdentity(*ExistingText, &ExistingNamespace, &ExistingKey))
    {
        OutText = FText::ChangeKey(
            FTextKey(*ExistingNamespace),
            FTextKey(*ExistingKey),
            FText::FromString(TextValue));
        return true;
    }

    OutError = TEXT("Persisted FText values require a non-empty namespace and key; pass NSLOCTEXT(\"Namespace\", \"Key\", \"Source\") or update an existing localized value");
    return false;
}

bool CoerceJsonValueToPersistedFText(
    const TSharedPtr<FJsonValue>& ValueField,
    const FText* ExistingText,
    FText& OutText,
    FString& OutError)
{
    OutError.Empty();
    if (!ValueField.IsValid())
    {
        OutError = TEXT("Invalid text value");
        return false;
    }
    if (ValueField->Type != EJson::String)
    {
        OutError = TEXT("Persisted FText values must be strings with a namespace and key");
        return false;
    }
    return CoerceStringToPersistedFText(ValueField->AsString(), ExistingText, OutText, OutError);
}

bool ApplyJsonValueToArrayDirect(FArrayProperty* AP, void* DirectArrayValue,
                                 const TSharedPtr<FJsonValue>& ValueField,
                                 FString& OutError)
{
    OutError.Empty();
    if (!AP || !DirectArrayValue || !ValueField)
    {
        OutError = TEXT("Invalid array property/value");
        return false;
    }
    if (ValueField->Type != EJson::Array)
    {
        OutError = TEXT("Expected array for array property");
        return false;
    }

    FScriptArrayHelper Helper(AP, DirectArrayValue);
    TArray<FText> ExistingTextValues;
    if (CastField<FTextProperty>(AP->Inner))
    {
        ExistingTextValues.Reserve(Helper.Num());
        for (int32 ExistingIndex = 0; ExistingIndex < Helper.Num(); ++ExistingIndex)
        {
            ExistingTextValues.Add(*reinterpret_cast<FText*>(Helper.GetRawPtr(ExistingIndex)));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>& Src = ValueField->AsArray();
    FScriptArray ScratchArray;
    FScriptArrayHelper ScratchHelper(AP, &ScratchArray);
    struct FArrayScratchCleanup
    {
        FScriptArrayHelper& Helper;
        ~FArrayScratchCleanup()
        {
            Helper.EmptyValues();
        }
    } ScratchCleanup{ScratchHelper};

    for (int32 i = 0; i < Src.Num(); ++i)
    {
        ScratchHelper.AddValue();
        void* ElemPtr = ScratchHelper.GetRawPtr(ScratchHelper.Num() - 1);
        FProperty* Inner = AP->Inner;
        const TSharedPtr<FJsonValue>& V = Src[i];

        if (IsJsonScalarProperty(Inner))
        {
            void* StagedValue = nullptr;
            FString ScalarError;
            if (!TryStageJsonValueForProperty(Inner, V, StagedValue, ScalarError))
            {
                OutError = FString::Printf(TEXT("Array element %d: %s"), i, *ScalarError);
                return false;
            }
            Inner->CopySingleValue(ElemPtr, StagedValue);
            Inner->DestroyValue(StagedValue);
            FMemory::Free(StagedValue);
            continue;
        }

        if (FStrProperty* SIP = CastField<FStrProperty>(Inner))
        {
            FString& Dest = *reinterpret_cast<FString*>(ElemPtr);
            Dest = (V->Type == EJson::String)
                       ? V->AsString()
                       : FString::Printf(TEXT("%g"), V->AsNumber());
            continue;
        }
        if (FNameProperty* NIP = CastField<FNameProperty>(Inner))
        {
            FName& Dest = *reinterpret_cast<FName*>(ElemPtr);
            Dest = (V->Type == EJson::String)
                       ? FName(*V->AsString())
                       : FName(*FString::Printf(TEXT("%g"), V->AsNumber()));
            continue;
        }
        if (FTextProperty* TIP = CastField<FTextProperty>(Inner))
        {
            FText& Dest = *reinterpret_cast<FText*>(ElemPtr);
            const FText* ExistingText = ExistingTextValues.IsValidIndex(i) ? &ExistingTextValues[i] : nullptr;
            if (!CoerceJsonValueToPersistedFText(V, ExistingText, Dest, OutError))
            {
                const FString ElementError = OutError;
                OutError = FString::Printf(TEXT("Text array element %d: %s"), i, *ElementError);
                return false;
            }
            continue;
        }
        if (FObjectProperty* ObjInner = CastField<FObjectProperty>(Inner))
        {
            // Mirror the scalar FObjectProperty branch (665-706): fail loud
            // instead of silently storing null. Only a JSON string (an asset
            // path or a {"", "None", "null"} clear-sentinel) or JSON null is a
            // valid element; any other JSON type (object/number/bool/nested
            // array) is NOT a silent "clear to null", and a non-sentinel path
            // that fails to load is an error — both must surface as applied:false
            // rather than a bogus null stored under a reported success.
            if (V->Type != EJson::String && V->Type != EJson::Null)
            {
                OutError = FString::Printf(
                    TEXT("Array element %d: unsupported JSON type for object property (expected an asset path string or null)"),
                    i);
                return false;
            }
            UObject* LoadedObj = nullptr;
            if (V->Type == EJson::String)
            {
                const FString ObjPath = V->AsString();
                // {"", "None", "null"} clear the element to null; only a real path loads.
                if (!IsNullObjectSentinel(ObjPath))
                {
                    // Reject struct-text blobs / over-length strings before they
                    // reach FName inside LoadObject — see IsPlausibleObjectPathForLoad
                    // (the scalar branch applies the same guard at 678). Without it a
                    // >= NAME_SIZE or struct-text array element aborts the editor.
                    if (!IsPlausibleObjectPathForLoad(ObjPath))
                    {
                        OutError = FString::Printf(
                            TEXT("Array element %d: object property value is not a valid path (length=%d, contains struct-text punctuation or exceeds FName limit)"),
                            i, ObjPath.Len());
                        return false;
                    }
                    LoadedObj = LoadObject<UObject>(nullptr, *ObjPath);
                    // Match the scalar branch (686): a fully-qualified dotted path is
                    // already handled by LoadObject, so only retry StaticLoadObject for
                    // name-only paths instead of a guaranteed-redundant second load.
                    if (!LoadedObj && !ObjPath.Contains(TEXT("."))) LoadedObj = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjPath);
                    if (!LoadedObj)
                    {
                        OutError = FString::Printf(
                            TEXT("Array element %d: failed to load object at path: %s"),
                            i, *ObjPath);
                        return false;
                    }
                }
            }
            // EJson::Null and the string sentinels fall through with
            // LoadedObj == nullptr, preserving the legitimate clear-to-null.
            ObjInner->SetObjectPropertyValue(ElemPtr, LoadedObj);
            continue;
        }
        if (FStructProperty* StructInner = CastField<FStructProperty>(Inner))
        {
            // Each JSON-object element recurses field-by-field, mirroring the
            // nested struct-property branch above. Required for arrays like
            // UMaterialExpressionCustom::AdditionalOutputs (FCustomOutput).
            if (V->Type != EJson::Object)
            {
                OutError = FString::Printf(
                    TEXT("Array element %d: expected JSON object for struct property '%s', got %s"),
                    i, *StructInner->Struct->GetName(),
                    *FString::FromInt(static_cast<int32>(V->Type)));
                return false;
            }
            const TSharedPtr<FJsonObject>& ElemObj = V->AsObject();
            if (!ElemObj.IsValid() || !StructInner->Struct)
            {
                OutError = FString::Printf(TEXT("Array element %d: invalid struct payload"), i);
                return false;
            }
            FString SubErrors;
            bool bAllOk = true;
            for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : ElemObj->Values)
            {
                FProperty* SubProp = FindPropertyCI(StructInner->Struct, Pair.Key);
                if (!SubProp)
                {
                    bAllOk = false;
                    SubErrors += FString::Printf(TEXT("; unknown sub-field '%s'"), *Pair.Key);
                    continue;
                }
                FString SubError;
                if (!ApplyJsonValueToProperty(ElemPtr, SubProp, Pair.Value, SubError))
                {
                    bAllOk = false;
                    SubErrors += FString::Printf(TEXT("; '%s': %s"), *Pair.Key, *SubError);
                }
            }
            if (!bAllOk)
            {
                OutError = FString::Printf(
                    TEXT("Array element %d: failed to apply struct fields%s"),
                    i, *SubErrors);
                return false;
            }
            continue;
        }

        OutError = TEXT("Unsupported array inner property type for JSON assignment");
        return false;
    }

    Helper.EmptyAndAddValues(Src.Num());
    for (int32 i = 0; i < Src.Num(); ++i)
    {
        AP->Inner->CopySingleValue(Helper.GetRawPtr(i), ScratchHelper.GetRawPtr(i));
    }
    return true;
}

bool ApplyJsonValueToProperty(void* TargetContainer, FProperty* Property,
                              const TSharedPtr<FJsonValue>& ValueField,
                              FString& OutError)
{
    OutError.Empty();
    if (!TargetContainer || !Property || !ValueField)
    {
        OutError = TEXT("Invalid target/property/value");
        return false;
    }

    if (IsJsonScalarPropertyImpl(Property))
    {
        void* TargetValue = Property->ContainerPtrToValuePtr<void>(TargetContainer);
        return ApplyJsonValueToPropertyDirect(TargetValue, Property, ValueField, OutError);
    }

    // String and Name
    if (FStrProperty* SP = CastField<FStrProperty>(Property))
    {
        if (ValueField->Type == EJson::String)
        {
            SP->SetPropertyValue_InContainer(TargetContainer, ValueField->AsString());
            return true;
        }
        OutError = TEXT("Expected string for string property");
        return false;
    }
    if (FNameProperty* NP = CastField<FNameProperty>(Property))
    {
        if (ValueField->Type == EJson::String)
        {
            NP->SetPropertyValue_InContainer(TargetContainer, FName(*ValueField->AsString()));
            return true;
        }
        OutError = TEXT("Expected string for name property");
        return false;
    }
    if (FTextProperty* TP = CastField<FTextProperty>(Property))
    {
        if (ValueField->Type == EJson::Null)
        {
            TP->SetPropertyValue_InContainer(TargetContainer, FText::GetEmpty());
            return true;
        }

        const FText ExistingText = TP->GetPropertyValue_InContainer(TargetContainer);
        FText NewText;
        if (!CoerceJsonValueToPersistedFText(ValueField, &ExistingText, NewText, OutError))
        {
            return false;
        }

        TP->SetPropertyValue_InContainer(TargetContainer, NewText);
        return true;
    }

    // Reflected bool/numeric/enum fields use the same direct-pointer conversion for both
    // ordinary writes and scratch staging. Keeping this dispatch before object/struct
    // branches prevents the two scalar ladders from drifting apart.

    // Class reference (TSubclassOf<> / UClass*) — must come BEFORE FObjectProperty
    // since FClassProperty inherits from FObjectProperty.
    if (FClassProperty* CP = CastField<FClassProperty>(Property))
    {
        if (ValueField->Type == EJson::String)
        {
            const FString Path = ValueField->AsString();
            UClass* ClassObj = nullptr;
            // {"", "None", "null"} clear the reference; anything else is a path.
            if (!IsNullObjectSentinel(Path))
            {
                // Reject struct-text blobs / over-length strings before they
                // reach FName inside LoadObject — see IsPlausibleObjectPathForLoad.
                if (!IsPlausibleObjectPathForLoad(Path))
                {
                    OutError = FString::Printf(
                        TEXT("Class property value is not a valid path (length=%d, contains struct-text punctuation or exceeds FName limit)"),
                        Path.Len());
                    return false;
                }
                // For non-native paths, resolve to generated class (_C suffix) first
                // since Blueprint paths always need it. Native /Script/ paths load as-is.
                FString AttemptPath = Path;
                if (!Path.StartsWith(TEXT("/Script/")) && !Path.EndsWith(TEXT("_C")))
                {
                    if (!Path.Contains(TEXT(".")))
                    {
                        const FString AssetName = FPackageName::GetShortName(Path);
                        AttemptPath = FString::Printf(TEXT("%s.%s_C"), *Path, *AssetName);
                    }
                    else
                    {
                        AttemptPath += TEXT("_C");
                    }
                }
                ClassObj = LoadObject<UClass>(nullptr, *AttemptPath);
                if (!ClassObj && AttemptPath != Path)
                {
                    // Fallback to original path (e.g., already-resolved or non-standard format)
                    ClassObj = LoadObject<UClass>(nullptr, *Path);
                }
                if (!ClassObj)
                {
                    OutError = FString::Printf(TEXT("Failed to resolve class at path: %s"), *Path);
                    return false;
                }
                if (CP->MetaClass && !ClassObj->IsChildOf(CP->MetaClass))
                {
                    OutError = FString::Printf(TEXT("Class '%s' is not a child of '%s'"),
                        *ClassObj->GetName(), *CP->MetaClass->GetName());
                    return false;
                }
            }
            CP->SetObjectPropertyValue_InContainer(TargetContainer, ClassObj);
            return true;
        }
        if (ValueField->Type == EJson::Null)
        {
            CP->SetObjectPropertyValue_InContainer(TargetContainer, nullptr);
            return true;
        }
        OutError = TEXT("Class property requires string path or null");
        return false;
    }

    // Object reference
    if (FObjectProperty* OP = CastField<FObjectProperty>(Property))
    {
        if (ValueField->Type == EJson::String)
        {
            const FString Path = ValueField->AsString();
            UObject* Res = nullptr;
            // {"", "None", "null"} clear the reference to null; "None" must NOT
            // reach LoadObject (it would fail with the misleading
            // "Failed to load object at path: None").
            if (!IsNullObjectSentinel(Path))
            {
                // Reject struct-text blobs / over-length strings before they
                // reach FName inside LoadObject — see IsPlausibleObjectPathForLoad.
                if (!IsPlausibleObjectPathForLoad(Path))
                {
                    OutError = FString::Printf(
                        TEXT("Object property value is not a valid path (length=%d, contains struct-text punctuation or exceeds FName limit)"),
                        Path.Len());
                    return false;
                }
                Res = LoadObject<UObject>(nullptr, *Path);
                if (!Res && !Path.Contains(TEXT(".")))
                {
                    Res = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
                }
                if (!Res)
                {
                    OutError = FString::Printf(TEXT("Failed to load object at path: %s"), *Path);
                    return false;
                }
            }
            OP->SetObjectPropertyValue_InContainer(TargetContainer, Res);
            return true;
        }
        if (ValueField->Type == EJson::Null)
        {
            OP->SetObjectPropertyValue_InContainer(TargetContainer, nullptr);
            return true;
        }
        OutError = TEXT("Unsupported JSON type for object property");
        return false;
    }

    // Soft class references — must come BEFORE FSoftObjectProperty since
    // FSoftClassProperty inherits from it (otherwise the value is stored unvalidated).
    if (FSoftClassProperty* SCP = CastField<FSoftClassProperty>(Property))
    {
        if (ValueField->Type == EJson::String)
        {
            const FString Path = ValueField->AsString();
            void* ValuePtr = SCP->ContainerPtrToValuePtr<void>(TargetContainer);
            FSoftObjectPtr* SoftClassPtr = static_cast<FSoftObjectPtr*>(ValuePtr);
            if (SoftClassPtr)
            {
                // {"", "None", "null"} clear the reference; without this guard
                // "None" was silently stored as the literal FSoftObjectPath("None").
                if (IsNullObjectSentinel(Path))
                {
                    *SoftClassPtr = FSoftObjectPtr();
                }
                else
                {
                    UClass* ClassObj = ResolveSoftClassValue(Path, OutError);
                    if (!ClassObj)
                    {
                        return false;
                    }
                    if (SCP->MetaClass && !ClassObj->IsChildOf(SCP->MetaClass))
                    {
                        OutError = FString::Printf(TEXT("Class '%s' is not a child of '%s'"),
                            *ClassObj->GetName(), *SCP->MetaClass->GetName());
                        return false;
                    }
                    *SoftClassPtr = FSoftObjectPath(ClassObj);
                }
                return true;
            }
            OutError = TEXT("Failed to access soft class property");
            return false;
        }
        else if (ValueField->Type == EJson::Null)
        {
            void* ValuePtr = SCP->ContainerPtrToValuePtr<void>(TargetContainer);
            FSoftObjectPtr* SoftClassPtr = static_cast<FSoftObjectPtr*>(ValuePtr);
            if (SoftClassPtr)
            {
                *SoftClassPtr = FSoftObjectPtr();
                return true;
            }
        }
        OutError = TEXT("Soft class property requires string path or null");
        return false;
    }

    // Soft object references
    if (FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Property))
    {
        if (ValueField->Type == EJson::String)
        {
            const FString Path = ValueField->AsString();
            void* ValuePtr = SOP->ContainerPtrToValuePtr<void>(TargetContainer);
            FSoftObjectPtr* SoftObjPtr = static_cast<FSoftObjectPtr*>(ValuePtr);
            if (SoftObjPtr)
            {
                // {"", "None", "null"} clear the reference; without this guard
                // "None" was silently stored as the literal FSoftObjectPath("None").
                if (IsNullObjectSentinel(Path))
                {
                    *SoftObjPtr = FSoftObjectPtr();
                }
                else
                {
                    *SoftObjPtr = FSoftObjectPath(Path);
                }
                return true;
            }
            OutError = TEXT("Failed to access soft object property");
            return false;
        }
        else if (ValueField->Type == EJson::Null)
        {
            void* ValuePtr = SOP->ContainerPtrToValuePtr<void>(TargetContainer);
            FSoftObjectPtr* SoftObjPtr = static_cast<FSoftObjectPtr*>(ValuePtr);
            if (SoftObjPtr)
            {
                *SoftObjPtr = FSoftObjectPtr();
                return true;
            }
        }
        OutError = TEXT("Soft object property requires string path or null");
        return false;
    }

    // Structs (Vector/Rotator/LinearColor)
    if (FStructProperty* SP = CastField<FStructProperty>(Property))
    {
        const FString TypeName = SP->Struct ? SP->Struct->GetName() : FString();
        if (ValueField->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
            // FVector / FRotator components are `double` under LWC, and AsNumber()
            // already carries the JSON number as a double. Narrowing through float
            // here silently rewrote every fractional component to its float32
            // neighbour (0.18 -> 0.18000000715255737) on the array literal form
            // only: the object form below recurses into the FDoubleProperty
            // sub-properties and stored the exact value, so `[0.18,..]` and
            // `{"x":0.18,..}` disagreed, and an export (which emits vectors as
            // arrays, PropertyExport.cpp Vector case) could not round-trip. The
            // shared JSON->vector reader in JsonUtils.cpp (ReadVectorFieldImpl)
            // never narrowed; this branch was the outlier. FLinearColor below keeps
            // its casts because its components really are float.
            if (TypeName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase) && Arr.Num() >= 3)
            {
                FVector V(Arr[0]->AsNumber(), Arr[1]->AsNumber(),
                          Arr[2]->AsNumber());
                SP->Struct->CopyScriptStruct(
                    SP->ContainerPtrToValuePtr<void>(TargetContainer), &V);
                return true;
            }
            if (TypeName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase) && Arr.Num() >= 3)
            {
                FRotator R(Arr[0]->AsNumber(), Arr[1]->AsNumber(),
                           Arr[2]->AsNumber());
                SP->Struct->CopyScriptStruct(
                    SP->ContainerPtrToValuePtr<void>(TargetContainer), &R);
                return true;
            }
            if (TypeName.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase) && Arr.Num() >= 3)
            {
                FLinearColor Color(
                    (float)Arr[0]->AsNumber(),
                    (float)Arr[1]->AsNumber(),
                    (float)Arr[2]->AsNumber(),
                    Arr.Num() >= 4 ? (float)Arr[3]->AsNumber() : 1.0f);
                SP->Struct->CopyScriptStruct(
                    SP->ContainerPtrToValuePtr<void>(TargetContainer), &Color);
                return true;
            }
        }

        // Fallback: nested JSON object -> recursively apply to each sub-property of the struct.
        // Enables shapes like {"Offsets": {"Left": 10, ...}} for CanvasPanelSlot.LayoutData.
        if (ValueField->Type == EJson::Object && SP->Struct)
        {
            const TSharedPtr<FJsonObject>& Obj = ValueField->AsObject();
            if (Obj.IsValid())
            {
                void* StructValuePtr = SP->ContainerPtrToValuePtr<void>(TargetContainer);
                bool bAllOk = true;
                FString SubErrors;
                for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Obj->Values)
                {
                    FProperty* SubProp = FindPropertyCI(SP->Struct, Pair.Key);
                    if (!SubProp)
                    {
                        bAllOk = false;
                        SubErrors += FString::Printf(TEXT("; unknown sub-field '%s'"), *Pair.Key);
                        continue;
                    }
                    FString SubError;
                    if (!ApplyJsonValueToProperty(StructValuePtr, SubProp, Pair.Value, SubError))
                    {
                        bAllOk = false;
                        SubErrors += FString::Printf(TEXT("; '%s': %s"), *Pair.Key, *SubError);
                    }
                }
                if (bAllOk)
                {
                    return true;
                }
                OutError = FString::Printf(
                    TEXT("Failed to apply nested JSON object to struct property '%s'%s"),
                    *Property->GetName(), *SubErrors);
                return false;
            }
        }

        if (ValueField->Type == EJson::String)
        {
            const FString Txt = ValueField->AsString();
            if (SP->Struct)
            {
                // First attempt: parse as JSON and convert to struct
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Txt);
                TSharedPtr<FJsonObject> ParsedObj;
                if (FJsonSerializer::Deserialize(Reader, ParsedObj) && ParsedObj.IsValid())
                {
                    if (FJsonObjectConverter::JsonObjectToUStruct(
                            ParsedObj.ToSharedRef(), SP->Struct,
                            SP->ContainerPtrToValuePtr<void>(TargetContainer), 0, 0))
                    {
                        return true;
                    }
                }

                // Last resort: ExportText-style literal like "(Offsets=(Left=10,...))"
                FString ImportError;
                if (ImportTextToProperty(TargetContainer, Property, Txt, ImportError))
                {
                    return true;
                }
                OutError = FString::Printf(
                    TEXT("Unsupported string value for struct property '%s' (ImportText fallback: %s)"),
                    *Property->GetName(), *ImportError);
                return false;
            }
        }

        OutError = TEXT("Unsupported JSON type for struct property");
        return false;
    }

    // Arrays
    if (FArrayProperty* AP = CastField<FArrayProperty>(Property))
    {
        return ApplyJsonValueToArrayDirect(
            AP, AP->ContainerPtrToValuePtr<void>(TargetContainer), ValueField, OutError);
    }
    OutError = TEXT("Unsupported property type for JSON assignment");
    return false;
}

bool ImportTextToProperty(void* TargetContainer, FProperty* Property,
                          const FString& TextValue, FString& OutError)
{
    OutError.Empty();
    if (!TargetContainer || !Property)
    {
        OutError = TEXT("Invalid target or property");
        return false;
    }

    void* ValuePtr = Property->ContainerPtrToValuePtr<void>(TargetContainer);
    if (!ValuePtr)
    {
        OutError = TEXT("Failed to resolve property value pointer");
        return false;
    }

    void* ScratchValue = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
    if (!ScratchValue)
    {
        OutError = FString::Printf(TEXT("Failed to allocate scratch storage for property '%s'"),
                                   *Property->GetName());
        return false;
    }

    Property->InitializeValue(ScratchValue);
    const TCHAR* Result = Property->ImportText_Direct(*TextValue, ScratchValue, nullptr, PPF_None);
    const bool bFullyConsumed = Result && *Result == TEXT('\0');
    if (!Result || !bFullyConsumed)
    {
        OutError = !Result
            ? FString::Printf(TEXT("ImportText failed for property '%s' with value '%s'"),
                              *Property->GetName(), *TextValue)
            : FString::Printf(TEXT("ImportText left trailing input for property '%s' with value '%s'"),
                              *Property->GetName(), *TextValue);
        Property->DestroyValue(ScratchValue);
        FMemory::Free(ScratchValue);
        return false;
    }

    Property->CopySingleValue(ValuePtr, ScratchValue);
    Property->DestroyValue(ScratchValue);
    FMemory::Free(ScratchValue);
    return true;
}
