// Copyright (c) 2026 Alexander Penkin. MIT License.

// JSON / text to property assignment utilities for PinWright
#include "Utils/PropertyImport.h"

#include "Utils/JsonUtils.h"
#include "Utils/PropertyInspection.h"

#include "Compat/EngineVersionCompat.h"
#include "Compat/JsonKeyCompat.h"
#include "Components/ActorComponent.h"
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
#include "Misc/ScopeExit.h"

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
            // Not valid JSON: the widget-XML exporter's `{Key=Value,...}` hybrid stays a
            // string; ApplyJsonValueToProperty's struct string branch rewrites it to an
            // ExportText literal for every caller (widget.set too), see
            // PropertyImportBraceHelpers::BraceHybridToExportText.
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

bool IsInstancedSubobjectText(const FProperty* Property, const TSharedPtr<FJsonValue>& ValueField)
{
    const FObjectProperty* OP = CastField<FObjectProperty>(Property);
    return OP && ValueField.IsValid() && ValueField->Type == EJson::String
        && OP->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance)
        && ValueField->AsString().TrimStartAndEnd().StartsWith(TEXT("{"));
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
        // Keep the existing identity but take the parsed source string, so identity-less
        // macros (INVTEXT, namespace-less LOCTEXT) store their payload, not their syntax.
        // Plain strings parse to themselves.
        OutText = FText::ChangeKey(
            FTextKey(*ExistingNamespace),
            FTextKey(*ExistingKey),
            FText::FromString(ParsedText.ToString()));
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

// widget.export_xml flattens every JSON object to `{Key=Value,...}` (JsonValueToAttrString,
// EJson::Object case in Handlers/UI/WidgetXmlUtils.h). These helpers read that form back.
namespace PropertyImportBraceHelpers
{
    // Struct text: swapping the structural braces for parens gives the ExportText literal
    // `(Key=Value,...)` that ImportText_Direct consumes.
    // KNOWN LIMITATION: a blind character swap, not grammar-aware. A string/text leaf holding a
    // brace (an FText `{0}` placeholder) is swapped too, and an array field (exported as bare
    // comma-joined elements, EJson::Array case) cannot be reconstructed by ImportText_Direct.
    FString BraceHybridToExportText(const FString& Trimmed)
    {
        FString Out = Trimmed;
        for (TCHAR& Ch : Out)
        {
            if (Ch == TEXT('{')) { Ch = TEXT('('); }
            else if (Ch == TEXT('}')) { Ch = TEXT(')'); }
        }
        return Out;
    }

    // Parse `{Key=Value,...}` at Text[Pos] into a JSON object with string leaves; nested
    // `{...}` values become nested objects. Returns null on malformed input or nesting deeper
    // than 64 levels.
    // ponytail: a leaf ends at the first ',' or '}' outside parens, so a flattened array leaf
    // is mis-split; quote leaves in the exporter if a real round-trip needs one.
    TSharedPtr<FJsonObject> ParseBraceHybrid(const FString& Text, int32& Pos, int32 Depth = 0)
    {
        if (Depth > 64 || Pos >= Text.Len() || Text[Pos] != TEXT('{'))
        {
            return nullptr;
        }
        ++Pos;
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        while (Pos < Text.Len() && Text[Pos] != TEXT('}'))
        {
            int32 Eq = Pos;
            while (Eq < Text.Len() && Text[Eq] != TEXT('=') && Text[Eq] != TEXT(',')
                && Text[Eq] != TEXT('{') && Text[Eq] != TEXT('}'))
            {
                ++Eq;
            }
            const FString Key = Text.Mid(Pos, Eq - Pos).TrimStartAndEnd();
            if (Eq >= Text.Len() || Text[Eq] != TEXT('=') || Key.IsEmpty())
            {
                return nullptr;
            }
            Pos = Eq + 1;
            if (Pos < Text.Len() && Text[Pos] == TEXT('{'))
            {
                TSharedPtr<FJsonObject> Child = ParseBraceHybrid(Text, Pos, Depth + 1);
                if (!Child)
                {
                    return nullptr;
                }
                Obj->SetObjectField(Key, Child);
            }
            else
            {
                const int32 ValueStart = Pos;
                int32 ParenDepth = 0;
                while (Pos < Text.Len()
                    && (ParenDepth > 0 || (Text[Pos] != TEXT(',') && Text[Pos] != TEXT('}'))))
                {
                    ParenDepth += Text[Pos] == TEXT('(') ? 1 : Text[Pos] == TEXT(')') ? -1 : 0;
                    ++Pos;
                }
                Obj->SetStringField(Key, Text.Mid(ValueStart, Pos - ValueStart));
            }
            if (Pos < Text.Len() && Text[Pos] == TEXT(','))
            {
                ++Pos;
            }
        }
        if (Pos >= Text.Len())
        {
            return nullptr;
        }
        ++Pos;
        return Obj;
    }

    // Drop the export's readback-only leaves that the importer cannot write and that carry no
    // authored value: an empty weak/lazy reference (a null one exports as "") and an unbound
    // delegate marker (`{_kind=FDelegateProperty,...,bindingStatus=empty}`, PropertyExport.cpp).
    // Every other leaf, empty or not, is applied: the export is a sparse diff, so an empty string
    // there is an authored empty value.
    void PruneUnwritableFields(const TSharedPtr<FJsonObject>& Obj, UStruct* Struct)
    {
        TArray<FString> Drop;
        for (const auto& Pair : Obj->Values)
        {
            const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
            FProperty* Field = FindPropertyCI(Struct, Key);
            if (!Field)
            {
                continue;
            }
            FString Status;
            if ((CastField<FWeakObjectProperty>(Field) || CastField<FLazyObjectProperty>(Field))
                && Pair.Value->Type == EJson::String && Pair.Value->AsString().IsEmpty())
            {
                Drop.Add(Key);
            }
            else if ((CastField<FDelegateProperty>(Field) || CastField<FMulticastDelegateProperty>(Field))
                && Pair.Value->Type == EJson::Object
                && Pair.Value->AsObject()->TryGetStringField(TEXT("bindingStatus"), Status) && Status == TEXT("empty"))
            {
                Drop.Add(Key);
            }
            else if (FStructProperty* StructField = CastField<FStructProperty>(Field))
            {
                if (Pair.Value->Type == EJson::Object)
                {
                    PruneUnwritableFields(Pair.Value->AsObject(), StructField->Struct);
                }
            }
        }
        for (const FString& Key : Drop)
        {
            Obj->RemoveField(Key);
        }
    }

    // An Instanced object property (UWidget::Navigation) exports as the subobject's sparse field
    // diff tagged `_kind=<class path>` (PropertyExport.cpp ExpandInstancedSubobject). Rebuild it as
    // a fresh subobject of that class outered to the owning object, each field applied, and assign
    // it only once every field applied.
    bool ApplyInstancedSubobjectText(void* TargetContainer, FObjectProperty* Property,
                                     const FString& Text, FString& OutError)
    {
        // UActorComponent is DefaultToInstanced, so every component pointer carries
        // CPF_InstancedReference; a component built here would be unregistered and unowned by the
        // actor's component lists. Components are authored through the component/SCS verbs.
        if (Property->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
        {
            OutError = FString::Printf(
                TEXT("Component property '%s' cannot be rebuilt from text; add or edit the component with the component verbs"),
                *Property->GetName());
            return false;
        }
        UClass* OwnerClass = Property->GetOwner<UClass>();
        if (!OwnerClass)
        {
            OutError = FString::Printf(
                TEXT("Instanced property '%s' can only be rebuilt from text on a UObject, not inside a struct"),
                *Property->GetName());
            return false;
        }
        int32 Pos = 0;
        TSharedPtr<FJsonObject> Fields = ParseBraceHybrid(Text, Pos);
        if (!Fields || Pos != Text.Len())
        {
            OutError = FString::Printf(
                TEXT("Instanced property '%s' expects the export form {Field=Value,...,_kind=<class path>}"),
                *Property->GetName());
            return false;
        }
        UClass* Class = Property->PropertyClass;
        FString KindPath;
        if (Fields->TryGetStringField(TEXT("_kind"), KindPath))
        {
            Fields->RemoveField(TEXT("_kind"));
            Class = IsPlausibleObjectPathForLoad(KindPath) ? LoadObject<UClass>(nullptr, *KindPath) : nullptr;
        }
        if (!Class || !Class->IsChildOf(Property->PropertyClass) || Class->HasAnyClassFlags(CLASS_Abstract)
            || Class->IsChildOf(UActorComponent::StaticClass()))
        {
            OutError = FString::Printf(TEXT("_kind '%s' is not a concrete non-component %s class"),
                *KindPath, *Property->PropertyClass->GetName());
            return false;
        }
        PruneUnwritableFields(Fields, Class);

        // Same flags the UMG designer gives a new UWidgetNavigation (WidgetNavigationCustomization.cpp).
        UObject* Outer = static_cast<UObject*>(TargetContainer);
        EObjectFlags Flags = RF_Transactional;
        if (Outer->IsTemplate())
        {
            Flags |= Outer->GetMaskedFlags(RF_PropagateToSubObjects) | RF_DefaultSubObject;
        }
        UObject* Subobject = NewObject<UObject>(Outer, Class, NAME_None, Flags);
        for (const auto& Pair : Fields->Values)
        {
            const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
            FProperty* Field = FindPropertyCI(Class, Key);
            FString FieldError = Field ? FString() : TEXT("unknown field");
            if (!Field || !ApplyJsonValueToProperty(Subobject, Field, Pair.Value, FieldError))
            {
                Subobject->MarkAsGarbage();
                OutError = FString::Printf(TEXT("Instanced %s field '%s': %s"),
                    *Class->GetName(), *Key, *FieldError);
                return false;
            }
        }
        // The replaced subobject would otherwise linger under the owner as an unreferenced
        // subobject; Rename is transactional, so undo restores it. A CDO's constructor-made
        // default subobject stays put: FObjectInitializer finds it by name for later instances.
        // On a Blueprint CDO that is one inherited by name (archetype = a parent CDO's subobject);
        // one this importer built usually has no namesake under the parent CDO, so its archetype
        // is its class CDO. A native CDO's own subobjects also archetype to the class CDO, so
        // every one there is kept.
        if (UObject* Replaced = Property->GetObjectPropertyValue_InContainer(TargetContainer))
        {
            const bool bNamedCdoSubobject = Outer->HasAnyFlags(RF_ClassDefaultObject)
                && Replaced->HasAnyFlags(RF_DefaultSubObject)
                && (!Outer->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
                    || !Replaced->GetArchetype()->HasAnyFlags(RF_ClassDefaultObject));
            if (Replaced->GetOuter() == Outer && !bNamedCdoSubobject)
            {
                Replaced->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS);
            }
        }
        Property->SetObjectPropertyValue_InContainer(TargetContainer, Subobject);
        return true;
    }
}

namespace PropertyImportContainerHelpers
{
    // A standalone value of Prop holding ValueField, written through ApplyJsonValueToProperty
    // so set elements and map keys/values take every type that function takes. Null (and the
    // value freed) on failure; otherwise the caller releases it with FreeValue.
    static void* StageValue(FProperty* Prop, const TSharedPtr<FJsonValue>& ValueField, FString& OutError)
    {
        void* Staged = Prop->AllocateAndInitializeValue();
        // ApplyJsonValueToProperty takes a container and adds the property's offset (a map
        // value's offset is the pair's value offset), so hand it the base that lands on Staged.
        uint8* Container = static_cast<uint8*>(Staged) - Prop->GetOffset_ForInternal();
        if (ApplyJsonValueToProperty(Container, Prop, ValueField, OutError))
        {
            return Staged;
        }
        Prop->DestroyValue(Staged);
        FMemory::Free(Staged);
        return nullptr;
    }

    static void FreeValue(FProperty* Prop, void* Staged)
    {
        Prop->DestroyValue(Staged);
        FMemory::Free(Staged);
    }
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
        if (IsInstancedSubobjectText(OP, ValueField))
        {
            return PropertyImportBraceHelpers::ApplyInstancedSubobjectText(
                TargetContainer, OP, ValueField->AsString().TrimStartAndEnd(), OutError);
        }
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

                // Last resort: ExportText-style literal like "(Offsets=(Left=10,...))", or the
                // widget-XML exporter's `{Key=Value,...}` hybrid rewritten to one.
                const FString Trimmed = Txt.TrimStartAndEnd();
                const FString ExportText = Trimmed.StartsWith(TEXT("{"))
                    ? PropertyImportBraceHelpers::BraceHybridToExportText(Trimmed) : Txt;
                FString ImportError;
                if (ImportTextToProperty(TargetContainer, Property, ExportText, ImportError))
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

    // Sets and maps are built in a scratch value and copied over the property only once every
    // element converted, so a failure leaves the property exactly as it was.
    if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
    {
        if (ValueField->Type != EJson::Array)
        {
            OutError = TEXT("Expected a JSON array [element, ...] for set property");
            return false;
        }
        void* Scratch = SetProp->AllocateAndInitializeValue();
        ON_SCOPE_EXIT { SetProp->DestroyValue(Scratch); FMemory::Free(Scratch); };
        FScriptSetHelper Helper(SetProp, Scratch);
        const TArray<TSharedPtr<FJsonValue>>& Src = ValueField->AsArray();
        for (int32 i = 0; i < Src.Num(); ++i)
        {
            FString ElemError;
            void* Elem = PropertyImportContainerHelpers::StageValue(SetProp->ElementProp, Src[i], ElemError);
            if (!Elem)
            {
                OutError = FString::Printf(TEXT("Set element %d: %s"), i, *ElemError);
                return false;
            }
            const int32 Before = Helper.Num();
            Helper.AddElement(Elem);
            PropertyImportContainerHelpers::FreeValue(SetProp->ElementProp, Elem);
            if (Helper.Num() == Before)
            {
                OutError = FString::Printf(TEXT("Set element %d duplicates an earlier element"), i);
                return false;
            }
        }
        SetProp->CopyCompleteValue(SetProp->ContainerPtrToValuePtr<void>(TargetContainer), Scratch);
        return true;
    }
    if (FMapProperty* MapProp = CastField<FMapProperty>(Property))
    {
        if (ValueField->Type != EJson::Object || !ValueField->AsObject().IsValid())
        {
            OutError = TEXT("Expected a JSON object {\"key\": value, ...} for map property");
            return false;
        }
        void* Scratch = MapProp->AllocateAndInitializeValue();
        ON_SCOPE_EXIT { MapProp->DestroyValue(Scratch); FMemory::Free(Scratch); };
        FScriptMapHelper Helper(MapProp, Scratch);
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : ValueField->AsObject()->Values)
        {
            // JSON object keys are always strings; scalar keys (int, enum name, bool) parse
            // that spelling exactly as a scalar property written from a string does.
            FString ElemError;
            void* Key = PropertyImportContainerHelpers::StageValue(
                MapProp->KeyProp, MakeShared<FJsonValueString>(Pair.Key), ElemError);
            if (!Key)
            {
                OutError = FString::Printf(TEXT("Map key '%s': %s"), *Pair.Key, *ElemError);
                return false;
            }
            void* Value = PropertyImportContainerHelpers::StageValue(MapProp->ValueProp, Pair.Value, ElemError);
            if (!Value)
            {
                PropertyImportContainerHelpers::FreeValue(MapProp->KeyProp, Key);
                OutError = FString::Printf(TEXT("Map value for key '%s': %s"), *Pair.Key, *ElemError);
                return false;
            }
            const int32 Before = Helper.Num();
            Helper.AddPair(Key, Value);
            PropertyImportContainerHelpers::FreeValue(MapProp->KeyProp, Key);
            PropertyImportContainerHelpers::FreeValue(MapProp->ValueProp, Value);
            if (Helper.Num() == Before)
            {
                OutError = FString::Printf(TEXT("Map key '%s' resolves to the same key as an earlier entry"), *Pair.Key);
                return false;
            }
        }
        MapProp->CopyCompleteValue(MapProp->ContainerPtrToValuePtr<void>(TargetContainer), Scratch);
        return true;
    }

    OutError = FString::Printf(
        TEXT("Unsupported property type '%s' for JSON assignment (supported: bool, numeric, enum, string, name, text, object/class/soft references, structs, arrays, sets, maps)"),
        *Property->GetClass()->GetName());
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
