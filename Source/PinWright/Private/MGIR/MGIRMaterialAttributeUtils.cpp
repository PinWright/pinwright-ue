// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRMaterialAttributeUtils.h"

#include "Compat/EngineVersionCompat.h"
#include "MaterialExpressionIO.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"

namespace
{
bool NamesMatch(const FString& A, const FString& B)
{
    const FString TrimmedA = A.TrimStartAndEnd();
    const FString TrimmedB = B.TrimStartAndEnd();
    if (TrimmedA.Equals(TrimmedB, ESearchCase::IgnoreCase))
    {
        return true;
    }

    FString CompactA;
    FString CompactB;
    for (int32 Index = 0; Index < TrimmedA.Len(); ++Index)
    {
        const TCHAR Ch = TrimmedA[Index];
        if (!FChar::IsWhitespace(Ch))
        {
            CompactA.AppendChar(Ch);
        }
    }
    for (int32 Index = 0; Index < TrimmedB.Len(); ++Index)
    {
        const TCHAR Ch = TrimmedB[Index];
        if (!FChar::IsWhitespace(Ch))
        {
            CompactB.AppendChar(Ch);
        }
    }
    return CompactA.Equals(CompactB, ESearchCase::IgnoreCase);
}

bool TryParseGuid(const FString& Value, FGuid& OutGuid)
{
    const FString Trimmed = Value.TrimStartAndEnd();
    // FGuid::ParseExact indexes the string at fixed offsets (e.g. [8],[13],[18],[23] for
    // DigitsWithHyphens) WITHOUT a length precheck on UE 5.4, so feeding it a short string
    // such as a material attribute name ("BaseColor") asserts IsValidIndex in
    // FString::operator[] (UnrealString.h.inl) and crashes the editor. FGuid::Parse itself is
    // length-safe (it dispatches by Len() first), so call it unguarded; gate the explicit
    // ParseExact fallbacks by the exact length each format requires (Digits = 32,
    // DigitsWithHyphens = 36) so we never index past the end. 5.5+ length-checks internally.
    if (FGuid::Parse(Trimmed, OutGuid))
    {
        return true;
    }
    if (Trimmed.Len() == 32 && FGuid::ParseExact(Trimmed, EGuidFormats::Digits, OutGuid))
    {
        return true;
    }
    if (Trimmed.Len() == 36 && FGuid::ParseExact(Trimmed, EGuidFormats::DigitsWithHyphens, OutGuid))
    {
        return true;
    }
    return false;
}

bool TryMatchKnownAttribute(const FString& Name, UMaterial* Material, FGuid& OutAttributeID)
{
    const TArray<FGuid>& AttributeIDs = FMaterialAttributeDefinitionMap::GetOrderedVisibleAttributeList();
    for (const FGuid& AttributeID : AttributeIDs)
    {
        const FString StableName = FMaterialAttributeDefinitionMap::GetAttributeName(AttributeID);
        const FString DisplayName = FMaterialAttributeDefinitionMap::GetDisplayNameForMaterial(AttributeID, Material).ToString();
        if (NamesMatch(Name, StableName) || NamesMatch(Name, DisplayName))
        {
            OutAttributeID = AttributeID;
            return true;
        }
    }

    TArray<FMaterialCustomOutputAttributeDefintion> CustomAttributes;
    FMaterialAttributeDefinitionMap::GetCustomAttributeList(CustomAttributes);
    for (const FMaterialCustomOutputAttributeDefintion& Attribute : CustomAttributes)
    {
        if (NamesMatch(Name, Attribute.AttributeName))
        {
            OutAttributeID = Attribute.AttributeID;
            return true;
        }
    }

    return false;
}

void RebuildInputs(UMaterialExpressionSetMaterialAttributes* Expression)
{
    if (!Expression)
    {
        return;
    }

    Expression->Inputs.Reset();
    Expression->Inputs.Add(FMaterialAttributesInput());
    for (const FGuid& AttributeID : Expression->AttributeSetTypes)
    {
        FExpressionInput Input;
        Input.InputName = FName(*FMGIRMaterialAttributeUtils::GetStableAttributeName(AttributeID));
        Expression->Inputs.Add(Input);
    }
}
}

FString FMGIRMaterialAttributeUtils::GetStableAttributeName(const FGuid& AttributeID)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // The FGuid overload of GetCustomAttribute() was added in UE 5.4.
    const FMaterialCustomOutputAttributeDefintion* CustomAttribute =
        FMaterialAttributeDefinitionMap::GetCustomAttribute(AttributeID);
    if (CustomAttribute)
    {
        return CustomAttribute->AttributeName;
    }
#else
    // On 5.3 GetCustomAttribute() only takes a name; resolve by GUID via the custom list.
    TArray<FMaterialCustomOutputAttributeDefintion> CustomAttributes;
    FMaterialAttributeDefinitionMap::GetCustomAttributeList(CustomAttributes);
    for (const FMaterialCustomOutputAttributeDefintion& Attribute : CustomAttributes)
    {
        if (Attribute.AttributeID == AttributeID)
        {
            return Attribute.AttributeName;
        }
    }
#endif

    return FMaterialAttributeDefinitionMap::GetAttributeName(AttributeID);
}

TArray<FString> FMGIRMaterialAttributeUtils::GetStableAttributeNames(
    const UMaterialExpressionSetMaterialAttributes* Expression)
{
    TArray<FString> Names;
    if (!Expression)
    {
        return Names;
    }

    Names.Reserve(Expression->AttributeSetTypes.Num());
    for (const FGuid& AttributeID : Expression->AttributeSetTypes)
    {
        Names.Add(GetStableAttributeName(AttributeID));
    }
    return Names;
}

bool FMGIRMaterialAttributeUtils::TryResolveAttributeID(
    const FString& AttributeNameOrGuid,
    const UMaterialExpressionSetMaterialAttributes* Expression,
    FGuid& OutAttributeID)
{
    if (TryParseGuid(AttributeNameOrGuid, OutAttributeID))
    {
        return true;
    }

    UMaterial* Material = Expression ? Expression->Material : nullptr;
    return TryMatchKnownAttribute(AttributeNameOrGuid, Material, OutAttributeID);
}

bool FMGIRMaterialAttributeUtils::ApplyAttributeNames(
    UMaterialExpressionSetMaterialAttributes* Expression,
    const TArray<FString>& AttributeNames,
    FString& OutError)
{
    if (!Expression)
    {
        OutError = TEXT("SetMaterialAttributes expression is null.");
        return false;
    }

    TArray<FGuid> AttributeIDs;
    AttributeIDs.Reserve(AttributeNames.Num());
    for (const FString& AttributeName : AttributeNames)
    {
        FGuid AttributeID;
        if (!TryResolveAttributeID(AttributeName, Expression, AttributeID))
        {
            OutError = FString::Printf(TEXT("Unknown material attribute '%s'."), *AttributeName);
            return false;
        }
        AttributeIDs.Add(AttributeID);
    }

    Expression->AttributeSetTypes = MoveTemp(AttributeIDs);
    RebuildInputs(Expression);
    return true;
}

void FMGIRMaterialAttributeUtils::RebuildInputsFromAttributeSetTypes(
    UMaterialExpressionSetMaterialAttributes* Expression)
{
    RebuildInputs(Expression);
}

bool FMGIRMaterialAttributeUtils::TryGetWireInputName(
    const UMaterialExpressionSetMaterialAttributes* Expression,
    const FString& StableAttributeName,
    FString& OutInputName)
{
    if (!Expression)
    {
        return false;
    }

    for (int32 InputIndex = 0; InputIndex < Expression->Inputs.Num(); ++InputIndex)
    {
        const FString InputName = Expression->GetInputName(InputIndex).ToString();
        if (NamesMatch(StableAttributeName, InputName))
        {
            OutInputName = InputName;
            return true;
        }
    }

    FGuid AttributeID;
    if (!TryResolveAttributeID(StableAttributeName, Expression, AttributeID))
    {
        return false;
    }

    const int32 AttributeIndex = Expression->AttributeSetTypes.Find(AttributeID);
    if (AttributeIndex == INDEX_NONE)
    {
        return false;
    }

    const int32 InputIndex = AttributeIndex + 1;
    if (!Expression->Inputs.IsValidIndex(InputIndex))
    {
        return false;
    }

    OutInputName = Expression->GetInputName(InputIndex).ToString();
    return true;
}
