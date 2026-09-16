// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintEnumHelpers.cpp - Implementation of shared enum-pin literal/value helpers.
#include "Handlers/Blueprint/BlueprintEnumHelpers.h"


#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace BlueprintEnumHelpers
{

FString StripEnumScope(const FString& InName)
{
    FString Name = InName;
    int32 ScopeSep = INDEX_NONE;
    if (Name.FindLastChar(TEXT(':'), ScopeSep) && ScopeSep >= 0 && ScopeSep + 1 < Name.Len())
    {
        Name = Name.Mid(ScopeSep + 1);
    }
    return Name;
}

bool TryResolveEnumLiteralToValue(const UEnum* EnumType, const FString& InLiteral, int64& OutValue)
{
    OutValue = INDEX_NONE;
    if (!EnumType)
    {
        return false;
    }

    const FString Literal = InLiteral.TrimStartAndEnd();
    if (Literal.IsEmpty())
    {
        return false;
    }

    int64 NumericValue = 0;
    if (LexTryParseString(NumericValue, *Literal))
    {
        const int32 IndexByNumeric = EnumType->GetIndexByValue(NumericValue);
        if (IndexByNumeric != INDEX_NONE)
        {
            const FString IndexName = EnumType->GetNameStringByIndex(IndexByNumeric);
            if (!IsEnumMaxEntryName(IndexName))
            {
                OutValue = NumericValue;
                return true;
            }
        }
    }

    const int32 NumEnums = EnumType->NumEnums();
    for (int32 Index = 0; Index < NumEnums; ++Index)
    {
        const FString NameByIndex = EnumType->GetNameStringByIndex(Index);
        if (IsEnumMaxEntryName(NameByIndex))
        {
            continue;
        }

        const FString FullyQualifiedName = EnumType->GetNameByIndex(Index).ToString();
        const FString AuthoredName = EnumType->GetAuthoredNameStringByIndex(Index);
        const FString DisplayName = EnumType->GetDisplayNameTextByIndex(Index).ToString();

        if (Literal.Equals(NameByIndex, ESearchCase::IgnoreCase)
            || Literal.Equals(StripEnumScope(NameByIndex), ESearchCase::IgnoreCase)
            || Literal.Equals(FullyQualifiedName, ESearchCase::IgnoreCase)
            || Literal.Equals(StripEnumScope(FullyQualifiedName), ESearchCase::IgnoreCase)
            || Literal.Equals(AuthoredName, ESearchCase::IgnoreCase)
            || Literal.Equals(DisplayName, ESearchCase::IgnoreCase))
        {
            OutValue = EnumType->GetValueByIndex(Index);
            return true;
        }
    }

    return false;
}

bool TryApplyEnumPinDefaultValue(
    const UEdGraphSchema* Schema,
    UEdGraphPin* Pin,
    const FString& RequestedValue,
    FString& OutAppliedLiteral,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutAppliedLiteral.Reset();
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!Schema || !Pin)
    {
        OutErrorCode = TEXT("INVALID_ARGUMENT");
        OutErrorMessage = TEXT("Missing graph schema or target pin");
        return false;
    }

    UEnum* EnumType = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
    if (!EnumType)
    {
        OutErrorCode = TEXT("INVALID_PIN_TYPE");
        OutErrorMessage = TEXT("Pin is not enum-typed");
        return false;
    }

    int64 RequestedEnumValue = INDEX_NONE;
    if (!TryResolveEnumLiteralToValue(EnumType, RequestedValue, RequestedEnumValue))
    {
        OutErrorCode = TEXT("INVALID_ENUM_LITERAL");
        OutErrorMessage = FString::Printf(
            TEXT("Invalid enum literal '%s' for enum '%s'"),
            *RequestedValue,
            *EnumType->GetName());
        return false;
    }

    TArray<FString> CandidateLiterals;
    auto AddCandidate = [&CandidateLiterals](const FString& Candidate)
    {
        if (!Candidate.IsEmpty() && !CandidateLiterals.Contains(Candidate))
        {
            CandidateLiterals.Add(Candidate);
        }
    };

    const FString CanonicalByValue = EnumType->GetNameStringByValue(RequestedEnumValue);
    AddCandidate(CanonicalByValue);
    AddCandidate(StripEnumScope(CanonicalByValue));
    AddCandidate(EnumType->GetNameByValue(RequestedEnumValue).ToString());
    AddCandidate(RequestedValue.TrimStartAndEnd());
    AddCandidate(FString::Printf(TEXT("%lld"), RequestedEnumValue));

    for (const FString& CandidateLiteral : CandidateLiterals)
    {
        Schema->TrySetDefaultValue(*Pin, CandidateLiteral);

        int64 AppliedValue = INDEX_NONE;
        const bool bResolvedFromDefaultValue =
            TryResolveEnumLiteralToValue(EnumType, Pin->DefaultValue, AppliedValue);
        const bool bResolvedFromDefaultText =
            !bResolvedFromDefaultValue &&
            TryResolveEnumLiteralToValue(EnumType, Pin->DefaultTextValue.ToString(), AppliedValue);
        if ((bResolvedFromDefaultValue || bResolvedFromDefaultText)
            && AppliedValue == RequestedEnumValue)
        {
            OutAppliedLiteral = bResolvedFromDefaultValue
                ? Pin->DefaultValue
                : Pin->DefaultTextValue.ToString();
            return true;
        }
    }

    OutErrorCode = TEXT("VERIFICATION_FAILED");
    OutErrorMessage = FString::Printf(
        TEXT("Requested enum literal '%s' but pin default remained '%s' (text='%s')"),
        *RequestedValue,
        *Pin->DefaultValue,
        *Pin->DefaultTextValue.ToString());
    return false;
}

} // namespace BlueprintEnumHelpers
