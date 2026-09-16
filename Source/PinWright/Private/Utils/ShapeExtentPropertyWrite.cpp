// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ShapeExtentPropertyWrite.h"

#include "Utils/PropertyImport.h"

#include "Components/ActorComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "UObject/UnrealType.h"

namespace PinWright
{
namespace
{
    // All four size properties are declared `protected` (BoxComponent.h:25,
    // SphereComponent.h:24, CapsuleComponent.h:26 and :33), so GET_MEMBER_NAME_CHECKED
    // cannot name them from outside the class. The reflected name is the stable identity
    // anyway - it is what FindPropertyByName resolved to reach this code at all. Same
    // reason as SkinnedAssetPropertyName() in ComponentAssetPropertyWrite.cpp.
    const FName& BoxExtentPropertyName()
    {
        static const FName Name(TEXT("BoxExtent"));
        return Name;
    }

    const FName& SphereRadiusPropertyName()
    {
        static const FName Name(TEXT("SphereRadius"));
        return Name;
    }

    const FName& CapsuleHalfHeightPropertyName()
    {
        static const FName Name(TEXT("CapsuleHalfHeight"));
        return Name;
    }

    const FName& CapsuleRadiusPropertyName()
    {
        static const FName Name(TEXT("CapsuleRadius"));
        return Name;
    }

    // Resolves ValueField through the SHARED importer without leaving it stored: the value
    // is landed on the real property, read back out, and the previous value put back, so
    // the typed setter is the only thing that mutates the component. Restoring on failure
    // too, because a nested-object write to an FVector can apply some sub-fields and still
    // report false (PropertyImport.cpp, the FStructProperty object branch) - a partial
    // extent left behind by a rejected write is exactly the half-landed state the verb
    // must not produce.
    //
    // Nothing observes the intermediate value: handlers run to completion on the game
    // thread with no tick, no GC and no render sync between these statements, and the
    // physics body this file exists to fix is not re-read until the setter runs.
    template <typename ValueType>
    bool ResolveThroughImporter(UActorComponent* Component, FProperty* Property,
                                const TSharedPtr<FJsonValue>& ValueField,
                                ValueType& OutValue, FString& OutError)
    {
        ValueType* const ValuePtr = Property->ContainerPtrToValuePtr<ValueType>(Component);
        const ValueType Previous = *ValuePtr;
        const bool bImported = ApplyJsonValueToProperty(Component, Property, ValueField, OutError);
        OutValue = *ValuePtr;
        *ValuePtr = Previous;
        return bImported;
    }
}

EShapeExtentWrite ApplyShapeExtentProperty(
    UActorComponent* Component,
    FProperty* Property,
    const TSharedPtr<FJsonValue>& ValueField,
    FString& OutError)
{
    if (!Component || !Property)
    {
        return EShapeExtentWrite::NotApplicable;
    }

    const FName PropertyName = Property->GetFName();

    // Each branch re-checks the PROPERTY SHAPE before reading raw memory through it. If a
    // future engine version retypes one of these (FVector -> a variant, float -> double),
    // the check fails, the write falls through to the generic path, and the worst outcome
    // is the old stale-body bug rather than a reinterpreted pointer.
    if (UBoxComponent* Box = Cast<UBoxComponent>(Component))
    {
        if (PropertyName != BoxExtentPropertyName())
        {
            return EShapeExtentWrite::NotApplicable;
        }
        const FStructProperty* StructProp = CastField<FStructProperty>(Property);
        if (!StructProp || StructProp->Struct != TBaseStructure<FVector>::Get())
        {
            return EShapeExtentWrite::NotApplicable;
        }

        FVector Requested = FVector::ZeroVector;
        if (!ResolveThroughImporter(Component, Property, ValueField, Requested, OutError))
        {
            return EShapeExtentWrite::Failed;
        }
        // No read-back verification, unlike ComponentAssetPropertyWrite: these setters are
        // void and unconditional - they cannot decline - and the capsule pair below
        // legitimately stores a value different from the requested one, so an equality
        // check against the request would report a clamp as a failure.
        Box->SetBoxExtent(Requested);
        return EShapeExtentWrite::Applied;
    }

    if (USphereComponent* Sphere = Cast<USphereComponent>(Component))
    {
        if (PropertyName != SphereRadiusPropertyName())
        {
            return EShapeExtentWrite::NotApplicable;
        }
        if (!CastField<FFloatProperty>(Property))
        {
            return EShapeExtentWrite::NotApplicable;
        }

        float Requested = 0.0f;
        if (!ResolveThroughImporter(Component, Property, ValueField, Requested, OutError))
        {
            return EShapeExtentWrite::Failed;
        }
        Sphere->SetSphereRadius(Requested);
        return EShapeExtentWrite::Applied;
    }

    if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Component))
    {
        const bool bIsHalfHeight = PropertyName == CapsuleHalfHeightPropertyName();
        const bool bIsRadius = PropertyName == CapsuleRadiusPropertyName();
        if (!bIsHalfHeight && !bIsRadius)
        {
            return EShapeExtentWrite::NotApplicable;
        }
        if (!CastField<FFloatProperty>(Property))
        {
            return EShapeExtentWrite::NotApplicable;
        }

        float Requested = 0.0f;
        if (!ResolveThroughImporter(Component, Property, ValueField, Requested, OutError))
        {
            return EShapeExtentWrite::Failed;
        }
        // Both inline setters funnel into SetCapsuleSize, which reads the OTHER dimension
        // off the component - so writing both in one call is order-dependent in exactly
        // the way the engine's own two setters are, and the second write wins.
        if (bIsHalfHeight)
        {
            Capsule->SetCapsuleHalfHeight(Requested);
        }
        else
        {
            Capsule->SetCapsuleRadius(Requested);
        }
        return EShapeExtentWrite::Applied;
    }

    return EShapeExtentWrite::NotApplicable;
}
}
