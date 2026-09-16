// Copyright (c) 2026 Alexander Penkin. MIT License.

// Cross-object property diff / copy utilities for PinWright
#include "Utils/PropertyDiff.h"

#include "Utils/PropertyExport.h"

#include "UObject/UnrealType.h"

TSharedPtr<FJsonObject> BuildSparsePropertyDiffJson(
    UObject* Instance,
    UObject* Baseline,
    const TSet<FName>& SkipProperties)
{
    if (!Instance)
    {
        return MakeShared<FJsonObject>();
    }

    UClass* InstanceClass = Instance->GetClass();
    UObject* EffectiveBaseline = Baseline;
    if (!EffectiveBaseline || !EffectiveBaseline->GetClass()->IsChildOf(InstanceClass))
    {
        EffectiveBaseline = InstanceClass ? InstanceClass->GetDefaultObject() : nullptr;
    }

    // Shared collect/sort/diff/oversized skeleton; this caller emits each leaf as the
    // inheritance-annotated {type, value, ...} object via SetField (byte-identical to
    // the former SetObjectField).
    return BuildSparseFieldDiffJson(
        Instance,
        EffectiveBaseline,
        SkipProperties,
        CPF_None,
        [Instance, EffectiveBaseline, InstanceClass](FProperty* Property) -> TSharedPtr<FJsonValue>
        {
            TSharedPtr<FJsonObject> PropObj = ExportPropertyToJsonValueWithInheritance(
                Instance,
                EffectiveBaseline,
                Property,
                InstanceClass);
            return PropObj.IsValid() ? MakeShared<FJsonValueObject>(PropObj) : TSharedPtr<FJsonValue>();
        });
}

void CopyFilteredMatchingProperties(
    UObject* Source,
    UObject* Target,
    const FFilteredPropertyCopyOptions& Options)
{
    if (!Source || !Target)
    {
        return;
    }

    UClass* SourceClass = Source->GetClass();
    UClass* TargetClass = Target->GetClass();
    if (!SourceClass || !TargetClass)
    {
        return;
    }

    for (TFieldIterator<FProperty> It(SourceClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        FProperty* SourceProperty = *It;
        if (!SourceProperty)
        {
            continue;
        }

        const FName PropertyName = SourceProperty->GetFName();
        if (Options.SkipPropertyNames.Contains(PropertyName))
        {
            continue;
        }

        FProperty* TargetProperty = FindFProperty<FProperty>(TargetClass, PropertyName);
        if (!TargetProperty)
        {
            continue;
        }

        if (!SourceProperty->SameType(TargetProperty) || SourceProperty->ArrayDim != TargetProperty->ArrayDim)
        {
            continue;
        }

        if (Options.RequiredAnyFlags != CPF_None &&
            (!SourceProperty->HasAnyPropertyFlags(Options.RequiredAnyFlags) ||
             !TargetProperty->HasAnyPropertyFlags(Options.RequiredAnyFlags)))
        {
            continue;
        }

        if (Options.ExcludedAnyFlags != CPF_None &&
            (SourceProperty->HasAnyPropertyFlags(Options.ExcludedAnyFlags) ||
             TargetProperty->HasAnyPropertyFlags(Options.ExcludedAnyFlags)))
        {
            continue;
        }

        const void* SourceValue = SourceProperty->ContainerPtrToValuePtr<void>(Source);
        void* TargetValue = TargetProperty->ContainerPtrToValuePtr<void>(Target);
        TargetProperty->CopyCompleteValue(TargetValue, SourceValue);
    }
}

void AddSceneAttachmentPropertySkips(FFilteredPropertyCopyOptions& Options)
{
    Options.SkipPropertyNames.Add(FName(TEXT("AttachParent")));
    Options.SkipPropertyNames.Add(FName(TEXT("AttachChildren")));
    Options.SkipPropertyNames.Add(FName(TEXT("ClientAttachedChildren")));
    Options.SkipPropertyNames.Add(FName(TEXT("AttachSocketName")));
}
