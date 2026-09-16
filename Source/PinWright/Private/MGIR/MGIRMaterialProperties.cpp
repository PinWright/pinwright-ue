// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRMaterialProperties.h"

#include "Materials/Material.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace MGIRMaterialProperties
{
namespace
{
enum class EKind : uint8
{
    Enum,
    Bool,
    Float,
    Int,
};

struct FSpec
{
    // Reflected UPROPERTY name on UMaterial. Doubles as the MGIR token, so the text and
    // the field it writes cannot drift apart.
    const TCHAR* Name;
    EKind Kind;
    // Enum member prefix, so `BlendMode: Translucent` is accepted alongside the canonical
    // `BlendMode: BLEND_Translucent` that Emit produces. Null for non-enum kinds.
    const TCHAR* EnumMemberPrefix;
};

// The carried set. Every entry here is state that decides whether the renderer uses the
// graph MGIR just wrote at all:
//   MaterialDomain / BlendMode / ShadingModel / TwoSided / TranslucencyLightingMode
//     - the surface configuration; wrong values discard connected pins outright.
//   bUseMaterialAttributes
//     - selects the MaterialAttributes root pin over the individual pins. A material
//       authored through SetMaterialAttributes decompiles to `output MaterialAttributes:`
//       and, without this, recompiles into a material reading the individual pins, all
//       unconnected: a fully black master reporting success.
//   OpacityMaskClipValue
//     - the masked cutoff; losing it silently re-cuts every masked master at 0.3333.
//   bIsThinSurface
//     - Substrate thin-surface flag, and it disables subsurface profiles.
//   NumCustomizedUVs
//     - how many CustomizedUV pins exist. The decompiler emits `output CustomizedUV<n>`
//       lines; at the default 0 those pins are not exposed and the wires read as absent.
// Anything not listed is documented as not preserved in docs/wiki-src/material.mgir.md.
const FSpec Specs[] =
{
    { TEXT("MaterialDomain"),            EKind::Enum,  TEXT("MD_") },
    { TEXT("BlendMode"),                 EKind::Enum,  TEXT("BLEND_") },
    { TEXT("ShadingModel"),              EKind::Enum,  TEXT("MSM_") },
    { TEXT("TwoSided"),                  EKind::Bool,  nullptr },
    { TEXT("bIsThinSurface"),            EKind::Bool,  nullptr },
    { TEXT("TranslucencyLightingMode"),  EKind::Enum,  TEXT("TLM_") },
    { TEXT("bUseMaterialAttributes"),    EKind::Bool,  nullptr },
    { TEXT("OpacityMaskClipValue"),      EKind::Float, nullptr },
    { TEXT("NumCustomizedUVs"),          EKind::Int,   nullptr },
};

FProperty* FindMaterialProperty(const FSpec& Spec)
{
    return UMaterial::StaticClass()->FindPropertyByName(FName(Spec.Name));
}

// Accepts `bUseMaterialAttributes` and `UseMaterialAttributes` for the same slot. The
// engine's own naming is inconsistent (`TwoSided` has no prefix, `bIsThinSurface` does),
// and matching the reflected name exactly is what keeps the token honest - so absorb the
// prefix here rather than maintaining a second table of friendly aliases.
FString StripBoolPrefix(const FString& Name)
{
    if (Name.Len() >= 2 && Name[0] == TEXT('b') && FChar::IsUpper(Name[1]))
    {
        return Name.Mid(1);
    }
    return Name;
}

int32 FindSpecIndex(const FString& Name)
{
    const FString Trimmed = Name.TrimStartAndEnd();
    for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Specs)); ++Index)
    {
        if (Trimmed.Equals(Specs[Index].Name, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }

    const FString Stripped = StripBoolPrefix(Trimmed);
    for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Specs)); ++Index)
    {
        if (Stripped.Equals(StripBoolPrefix(Specs[Index].Name), ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }

    return INDEX_NONE;
}

UEnum* GetSpecEnum(const FSpec& Spec, FProperty* Property)
{
    if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        return ByteProperty->Enum;
    }
    if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        return EnumProperty->GetEnum();
    }
    return nullptr;
}

bool TryReadEnumValue(const UMaterial* Material, FProperty* Property, int64& OutValue)
{
    if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        OutValue = static_cast<int64>(ByteProperty->GetPropertyValue_InContainer(Material));
        return true;
    }
    if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        const FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
        if (!Underlying)
        {
            return false;
        }
        OutValue = Underlying->GetSignedIntPropertyValue(
            EnumProperty->ContainerPtrToValuePtr<void>(Material));
        return true;
    }
    return false;
}

bool TryWriteEnumValue(UMaterial* Material, FProperty* Property, int64 Value)
{
    if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        ByteProperty->SetPropertyValue_InContainer(Material, static_cast<uint8>(Value));
        return true;
    }
    if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
        if (!Underlying)
        {
            return false;
        }
        Underlying->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Material), Value);
        return true;
    }
    return false;
}

// `_MAX` / `_NUM` are sentinels, not selectable values. Round-tripping one would write a
// material that no renderer path handles, so refuse it on input rather than pass it along.
bool IsSentinelEnumName(const FString& EnumName)
{
    return EnumName.EndsWith(TEXT("_MAX"), ESearchCase::CaseSensitive)
        || EnumName.EndsWith(TEXT("_NUM"), ESearchCase::CaseSensitive);
}

FString FormatEnumValue(const UEnum* Enum, int64 Value)
{
    const FString Name = Enum ? Enum->GetNameStringByValue(Value) : FString();
    // A value with no reflected name is still carried, as the raw integer, because dropping
    // it is exactly the silent loss this module removes. ParseProperty accepts the integer
    // form back for the same reason.
    return Name.IsEmpty() ? FString::Printf(TEXT("%lld"), Value) : Name;
}

bool TryParseBool(const FString& Text, bool& OutValue)
{
    const FString Trimmed = Text.TrimStartAndEnd();
    if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase)
        || Trimmed.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
        || Trimmed == TEXT("1"))
    {
        OutValue = true;
        return true;
    }
    if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase)
        || Trimmed.Equals(TEXT("no"), ESearchCase::IgnoreCase)
        || Trimmed == TEXT("0"))
    {
        OutValue = false;
        return true;
    }
    return false;
}

// Strips an `EBlendMode::BLEND_Translucent` style scope, so a value pasted from C++ or from
// a property dump resolves the same as the canonical token.
FString StripEnumScope(const FString& Text)
{
    int32 ScopeIndex = INDEX_NONE;
    if (Text.FindLastChar(TEXT(':'), ScopeIndex) && ScopeIndex > 0 && Text[ScopeIndex - 1] == TEXT(':'))
    {
        return Text.Mid(ScopeIndex + 1);
    }
    return Text;
}

FResult MakeUnknownPropertyError(const FString& Name)
{
    return FResult::MakeError(
        TEXT("MGIR_UNKNOWN_PROPERTY"),
        FString::Printf(
            TEXT("Unknown MGIR material property '%s'. MGIR carries: %s."),
            *Name,
            *FString::Join(GetCarriedPropertyNames(), TEXT(", "))));
}

FResult MakeMissingReflectionError(const FSpec& Spec)
{
    return FResult::MakeError(
        TEXT("MGIR_PROPERTY_UNRESOLVED"),
        FString::Printf(
            TEXT("UMaterial has no reflected property '%s' of the expected type; MGIR cannot ")
            TEXT("round-trip it on this engine build."),
            Spec.Name));
}
}

TArray<FString> Emit(const UMaterial* Material)
{
    TArray<FString> Lines;
    if (!Material)
    {
        return Lines;
    }

    for (const FSpec& Spec : Specs)
    {
        FProperty* Property = FindMaterialProperty(Spec);
        if (!Property)
        {
            // Loud rather than absent: a missing property means the carried set has drifted
            // from the engine, and a silently shorter dump is the defect, not the fix.
            Lines.Add(FString::Printf(
                TEXT("# property %s: <unresolved - UMaterial has no such reflected property>"),
                Spec.Name));
            continue;
        }

        switch (Spec.Kind)
        {
        case EKind::Enum:
        {
            int64 Value = 0;
            if (!TryReadEnumValue(Material, Property, Value))
            {
                Lines.Add(FString::Printf(
                    TEXT("# property %s: <unresolved - not a reflected enum property>"),
                    Spec.Name));
                break;
            }
            Lines.Add(FString::Printf(
                TEXT("property %s: %s"),
                Spec.Name,
                *FormatEnumValue(GetSpecEnum(Spec, Property), Value)));
            break;
        }
        case EKind::Bool:
        {
            const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property);
            if (!BoolProperty)
            {
                Lines.Add(FString::Printf(
                    TEXT("# property %s: <unresolved - not a reflected bool property>"),
                    Spec.Name));
                break;
            }
            Lines.Add(FString::Printf(
                TEXT("property %s: %s"),
                Spec.Name,
                BoolProperty->GetPropertyValue_InContainer(Material) ? TEXT("true") : TEXT("false")));
            break;
        }
        case EKind::Float:
        {
            const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property);
            if (!FloatProperty)
            {
                Lines.Add(FString::Printf(
                    TEXT("# property %s: <unresolved - not a reflected float property>"),
                    Spec.Name));
                break;
            }
            Lines.Add(FString::Printf(
                TEXT("property %s: %s"),
                Spec.Name,
                *FString::SanitizeFloat(FloatProperty->GetPropertyValue_InContainer(Material))));
            break;
        }
        case EKind::Int:
        {
            const FIntProperty* IntProperty = CastField<FIntProperty>(Property);
            if (!IntProperty)
            {
                Lines.Add(FString::Printf(
                    TEXT("# property %s: <unresolved - not a reflected int property>"),
                    Spec.Name));
                break;
            }
            Lines.Add(FString::Printf(
                TEXT("property %s: %d"),
                Spec.Name,
                IntProperty->GetPropertyValue_InContainer(Material)));
            break;
        }
        }
    }

    return Lines;
}

FResult ParseProperty(const FString& Name, const FString& Value, FParsed& OutParsed)
{
    const int32 SpecIndex = FindSpecIndex(Name);
    if (SpecIndex == INDEX_NONE)
    {
        return MakeUnknownPropertyError(Name);
    }

    const FSpec& Spec = Specs[SpecIndex];
    FProperty* Property = FindMaterialProperty(Spec);
    if (!Property)
    {
        return MakeMissingReflectionError(Spec);
    }

    OutParsed = FParsed();
    OutParsed.SpecIndex = SpecIndex;

    const FString Trimmed = Value.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return FResult::MakeError(
            TEXT("MGIR_BAD_PROPERTY_VALUE"),
            FString::Printf(TEXT("Material property '%s' has an empty value."), Spec.Name));
    }

    switch (Spec.Kind)
    {
    case EKind::Enum:
    {
        UEnum* Enum = GetSpecEnum(Spec, Property);
        if (!Enum)
        {
            return MakeMissingReflectionError(Spec);
        }

        const FString Token = StripEnumScope(Trimmed);
        int64 Resolved = Enum->GetValueByNameString(Token, EGetByNameFlags::None);
        if (Resolved == INDEX_NONE && Spec.EnumMemberPrefix)
        {
            Resolved = Enum->GetValueByNameString(
                FString(Spec.EnumMemberPrefix) + Token,
                EGetByNameFlags::None);
        }
        if (Resolved == INDEX_NONE && Token.IsNumeric())
        {
            const int64 Numeric = FCString::Atoi64(*Token);
            if (Enum->IsValidEnumValue(Numeric))
            {
                Resolved = Numeric;
            }
        }

        if (Resolved == INDEX_NONE || IsSentinelEnumName(Enum->GetNameStringByValue(Resolved)))
        {
            TArray<FString> Accepted;
            for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
            {
                const FString EnumName = Enum->GetNameStringByIndex(Index);
                if (!EnumName.IsEmpty() && !IsSentinelEnumName(EnumName))
                {
                    Accepted.AddUnique(EnumName);
                }
            }
            return FResult::MakeError(
                TEXT("MGIR_BAD_PROPERTY_VALUE"),
                FString::Printf(
                    TEXT("Material property '%s' does not accept '%s'. Accepted: %s."),
                    Spec.Name,
                    *Trimmed,
                    *FString::Join(Accepted, TEXT(", "))));
        }

        OutParsed.EnumValue = Resolved;
        return FResult();
    }
    case EKind::Bool:
    {
        if (!TryParseBool(Trimmed, OutParsed.bBoolValue))
        {
            return FResult::MakeError(
                TEXT("MGIR_BAD_PROPERTY_VALUE"),
                FString::Printf(
                    TEXT("Material property '%s' expects true or false, got '%s'."),
                    Spec.Name,
                    *Trimmed));
        }
        return FResult();
    }
    case EKind::Float:
    {
        if (!Trimmed.IsNumeric())
        {
            return FResult::MakeError(
                TEXT("MGIR_BAD_PROPERTY_VALUE"),
                FString::Printf(
                    TEXT("Material property '%s' expects a number, got '%s'."),
                    Spec.Name,
                    *Trimmed));
        }
        OutParsed.FloatValue = FCString::Atof(*Trimmed);
        return FResult();
    }
    case EKind::Int:
    {
        if (!Trimmed.IsNumeric() || Trimmed.Contains(TEXT(".")))
        {
            return FResult::MakeError(
                TEXT("MGIR_BAD_PROPERTY_VALUE"),
                FString::Printf(
                    TEXT("Material property '%s' expects a whole number, got '%s'."),
                    Spec.Name,
                    *Trimmed));
        }
        OutParsed.IntValue = FCString::Atoi(*Trimmed);
        return FResult();
    }
    }

    return MakeUnknownPropertyError(Name);
}

FResult ApplyProperty(UMaterial* Material, const FParsed& Parsed)
{
    if (!Material)
    {
        return FResult::MakeError(TEXT("MGIR_INVALID_TARGET"), TEXT("Material is null."));
    }
    if (Parsed.SpecIndex < 0 || Parsed.SpecIndex >= static_cast<int32>(UE_ARRAY_COUNT(Specs)))
    {
        return FResult::MakeError(
            TEXT("MGIR_UNKNOWN_PROPERTY"),
            TEXT("Material property was not resolved before apply."));
    }

    const FSpec& Spec = Specs[Parsed.SpecIndex];
    FProperty* Property = FindMaterialProperty(Spec);
    if (!Property)
    {
        return MakeMissingReflectionError(Spec);
    }

    switch (Spec.Kind)
    {
    case EKind::Enum:
        if (!TryWriteEnumValue(Material, Property, Parsed.EnumValue))
        {
            return MakeMissingReflectionError(Spec);
        }
        return FResult();
    case EKind::Bool:
        if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            BoolProperty->SetPropertyValue_InContainer(Material, Parsed.bBoolValue);
            return FResult();
        }
        return MakeMissingReflectionError(Spec);
    case EKind::Float:
        if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
        {
            FloatProperty->SetPropertyValue_InContainer(Material, Parsed.FloatValue);
            return FResult();
        }
        return MakeMissingReflectionError(Spec);
    case EKind::Int:
        if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
        {
            IntProperty->SetPropertyValue_InContainer(Material, Parsed.IntValue);
            return FResult();
        }
        return MakeMissingReflectionError(Spec);
    }

    return MakeMissingReflectionError(Spec);
}

void FinalizeAppliedProperties(UMaterial* Material)
{
    if (!Material)
    {
        return;
    }

    Material->RebuildShadingModelField();
}

TArray<FString> GetCarriedPropertyNames()
{
    TArray<FString> Names;
    Names.Reserve(UE_ARRAY_COUNT(Specs));
    for (const FSpec& Spec : Specs)
    {
        Names.Add(Spec.Name);
    }
    return Names;
}
}
