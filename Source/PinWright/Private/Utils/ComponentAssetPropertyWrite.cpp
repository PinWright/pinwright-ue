// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ComponentAssetPropertyWrite.h"

#include "Utils/PropertyImport.h"

#include "Components/ActorComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Misc/App.h"
#include "UObject/UnrealType.h"

namespace PinWright
{
namespace
{
    // USkinnedMeshComponent::SkinnedAsset is declared `private` and UE_DEPRECATED
    // (SkinnedMeshComponent.h:283-285), so GET_MEMBER_NAME_CHECKED cannot name it
    // from outside the class. The reflected name is the stable identity anyway -
    // it is what FindPropertyByName resolved to reach this code at all.
    const FName& SkinnedAssetPropertyName()
    {
        static const FName Name(TEXT("SkinnedAsset"));
        return Name;
    }

    FString DescribeWrongClass(const TCHAR* PropertyName, const UObject* Value,
                               const TCHAR* ExpectedClass)
    {
        return FString::Printf(
            TEXT("%s expects a %s; '%s' is a %s"), PropertyName, ExpectedClass,
            Value ? *Value->GetPathName() : TEXT("<null>"),
            (Value && Value->GetClass()) ? *Value->GetClass()->GetName() : TEXT("<unknown>"));
    }

    // Mirrors IsNullObjectSentinel in Utils/PropertyImport.cpp:85-91 - the one set of
    // strings that means "clear this reference" rather than "load this path". Kept in
    // step with that function by the ClearSentinelsMatchImporter test.
    bool IsClearSentinel(const TSharedPtr<FJsonValue>& ValueField)
    {
        if (!ValueField.IsValid() || ValueField->Type == EJson::Null)
        {
            return true;
        }
        if (ValueField->Type != EJson::String)
        {
            return false;
        }
        const FString Trimmed = ValueField->AsString().TrimStartAndEnd();
        return Trimmed.IsEmpty()
            || Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase)
            || Trimmed.Equals(TEXT("null"), ESearchCase::IgnoreCase);
    }
}

EComponentAssetWrite ApplyComponentAssetProperty(
    UActorComponent* Component,
    FProperty* Property,
    const TSharedPtr<FJsonValue>& ValueField,
    FString& OutError)
{
    if (!Component || !Property)
    {
        return EComponentAssetWrite::NotApplicable;
    }

    FObjectProperty* ObjProp = CastField<FObjectProperty>(Property);
    if (!ObjProp)
    {
        return EComponentAssetWrite::NotApplicable;
    }

    UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(Component);
    USkinnedMeshComponent* SkinnedComp = Cast<USkinnedMeshComponent>(Component);

    const bool bIsStaticMesh =
        StaticMeshComp
        && ObjProp->GetFName() == UStaticMeshComponent::GetMemberNameChecked_StaticMesh();
    const bool bIsSkinnedAsset =
        SkinnedComp && ObjProp->GetFName() == SkinnedAssetPropertyName();

    if (!bIsStaticMesh && !bIsSkinnedAsset)
    {
        return EComponentAssetWrite::NotApplicable;
    }

    // RESOLVE THROUGH THE SHARED IMPORTER, NOT A PRIVATE PATH PARSER. Asset-path
    // syntax, the {"", "None", "null"} clear sentinels, the FName-length and
    // struct-text rejections, and every error string must stay byte-identical to a
    // normal property write - duplicating that ladder here is how the two drift.
    // So the value is resolved by landing it on the real property and immediately
    // taking it back out. Nothing observes the intermediate state: handlers run to
    // completion on the game thread with no tick, no GC and no render sync between
    // these two statements, and the shadow copy the engine ensures on is not read
    // until the setter below updates it.
    UObject* const PreviousValue = ObjProp->GetObjectPropertyValue_InContainer(Component);
    if (!ApplyJsonValueToProperty(Component, Property, ValueField, OutError))
    {
        return EComponentAssetWrite::Failed;
    }
    UObject* const RequestedValue = ObjProp->GetObjectPropertyValue_InContainer(Component);
    ObjProp->SetObjectPropertyValue_InContainer(Component, PreviousValue);

    // A caller who named an asset must not silently get a CLEARED slot. The importer
    // loads any UObject at the path and hands it to FObjectProperty, which performs no
    // class check of its own (FObjectProperty::SetObjectPropertyValueUnchecked,
    // PropertyObject.cpp:477) - so a wrong-class path either lands as a mismatched
    // pointer or lands as null depending on build configuration. Deciding it here, off
    // the request rather than off the stored value, makes the outcome identical in both
    // cases: only the documented {"", "None", "null", JSON null} sentinels clear.
    if (!RequestedValue && !IsClearSentinel(ValueField))
    {
        OutError = FString::Printf(
            TEXT("%s did not resolve to a %s asset"), *ObjProp->GetName(),
            ObjProp->PropertyClass ? *ObjProp->PropertyClass->GetName() : TEXT("compatible"));
        return EComponentAssetWrite::Failed;
    }

    if (bIsStaticMesh)
    {
        UStaticMesh* const Requested = Cast<UStaticMesh>(RequestedValue);
        if (RequestedValue && !Requested)
        {
            OutError = DescribeWrongClass(TEXT("StaticMesh"), RequestedValue, TEXT("StaticMesh asset"));
            return EComponentAssetWrite::Failed;
        }

        // SetStaticMesh -> SetStaticMeshInternal -> NotifyIfStaticMeshChanged()
        // (StaticMeshComponent.cpp:2335-2350) is the only public route that updates
        // KnownStaticMesh, because NotifyIfStaticMeshChanged is private.
        StaticMeshComp->SetStaticMesh(Requested);

        // VERIFY AGAINST THE ENGINE, NOT THE RETURN VALUE. SetStaticMesh returns
        // false for "already this mesh" AND for "refused" (Static mobility with the
        // world begun play, StaticMeshComponent.cpp:2374-2383). Only the component's
        // own state separates the two, and reporting `applied` off the bool would
        // call a refusal a success.
        if (StaticMeshComp->GetStaticMesh() != Requested)
        {
            OutError = FString::Printf(
                TEXT("StaticMesh was refused by SetStaticMesh (component still holds '%s'); a Static-mobility component cannot change mesh once the world has begun play"),
                StaticMeshComp->GetStaticMesh() ? *StaticMeshComp->GetStaticMesh()->GetPathName()
                                                : TEXT("None"));
            return EComponentAssetWrite::Failed;
        }

        // The cluster tree is NOT rebuilt by SetStaticMesh, and a mesh swap changes
        // every instance's bounds. Mirrors the StaticMesh branch of
        // UHierarchicalInstancedStaticMeshComponent::PostEditChangeChainProperty
        // (HierarchicalInstancedStaticMesh.cpp:2120-2129), including its
        // FApp::CanEverRender() guard and its synchronous, forced form - the details
        // panel takes exactly this path when a user swaps the mesh on a HISM.
        if (UHierarchicalInstancedStaticMeshComponent* Hism =
                Cast<UHierarchicalInstancedStaticMeshComponent>(StaticMeshComp))
        {
            if (FApp::CanEverRender())
            {
                Hism->BuildTreeIfOutdated(/*Async*/ false, /*ForceUpdate*/ true);
            }
        }
        return EComponentAssetWrite::Applied;
    }

    USkinnedAsset* const Requested = Cast<USkinnedAsset>(RequestedValue);
    if (RequestedValue && !Requested)
    {
        OutError = DescribeWrongClass(TEXT("SkinnedAsset"), RequestedValue, TEXT("SkinnedAsset"));
        return EComponentAssetWrite::Failed;
    }

    // bReinitPose=true matches the engine default (SkinnedMeshComponent.h:1208) and
    // the details-panel behaviour: a new skeleton with the old pose retained is not
    // a state any editor path produces.
    SkinnedComp->SetSkinnedAssetAndUpdate(Requested, /*bReinitPose*/ true);
    if (SkinnedComp->GetSkinnedAsset() != Requested)
    {
        OutError = FString::Printf(
            TEXT("SkinnedAsset was refused by SetSkinnedAssetAndUpdate (component still holds '%s')"),
            SkinnedComp->GetSkinnedAsset() ? *SkinnedComp->GetSkinnedAsset()->GetPathName()
                                           : TEXT("None"));
        return EComponentAssetWrite::Failed;
    }
    return EComponentAssetWrite::Applied;
}
}
