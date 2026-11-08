// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ComponentReadFilter.h"

#include "Utils/ClassUtils.h"

#include "Components/ActorComponent.h"

namespace
{
    FString GetStringFirstOf(const TSharedPtr<FJsonObject>& Payload, const TArray<FString>& Keys)
    {
        if (!Payload.IsValid())
        {
            return FString();
        }

        for (const FString& Key : Keys)
        {
            FString Value;
            if (Payload->TryGetStringField(Key, Value))
            {
                Value.TrimStartAndEndInline();
                if (!Value.IsEmpty())
                {
                    return Value;
                }
            }
        }

        return FString();
    }
}

bool FComponentReadFilter::Matches(const UActorComponent* Component) const
{
    return Component
        && Matches(Component->GetName(), Component->GetClass());
}

bool FComponentReadFilter::Matches(const FString& ComponentName, const UClass* CandidateClass) const
{
    if (!NameMatch.IsEmpty()
        && !ComponentName.Contains(NameMatch, ESearchCase::IgnoreCase))
    {
        return false;
    }

    if (ComponentClass)
    {
        if (!CandidateClass || !CandidateClass->IsChildOf(ComponentClass))
        {
            return false;
        }
    }

    return true;
}

bool TryParseComponentReadFilter(
    const TSharedPtr<FJsonObject>& Payload,
    FComponentReadFilter& OutFilter,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutFilter = FComponentReadFilter();
    OutErrorCode.Empty();
    OutErrorMessage.Empty();

    if (!Payload.IsValid())
    {
        return true;
    }

    OutFilter.NameMatch = GetStringFirstOf(Payload, {TEXT("nameMatch"), TEXT("name_match")});

    const FString ClassName = GetStringFirstOf(Payload, {TEXT("componentClass"), TEXT("component_class")});
    if (!ClassName.IsEmpty())
    {
        UClass* ResolvedClass = ResolveClassByName(ClassName);
        if (!ResolvedClass)
        {
            OutErrorCode = TEXT("CLASS_NOT_FOUND");
            OutErrorMessage = FString::Printf(TEXT("Component class not found: %s"), *ClassName);
            return false;
        }

        if (!ResolvedClass->IsChildOf(UActorComponent::StaticClass()))
        {
            OutErrorCode = TEXT("INVALID_ARGUMENT");
            OutErrorMessage = FString::Printf(
                TEXT("componentClass must derive from UActorComponent: %s"),
                *ClassName);
            return false;
        }

        OutFilter.ComponentClass = ResolvedClass;
    }

    return true;
}
