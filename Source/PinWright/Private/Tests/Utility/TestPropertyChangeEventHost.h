// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "TestPropertyChangeEventHost.generated.h"

// Fixture types for TestPropertyChangeEventShape.cpp. The host records the SHAPE of
// every FPropertyChangedEvent it is handed, which is the only way to observe the
// defect in B-property-set-container-empty-change-event: a bare PostEditChange()
// still calls PostEditChangeProperty, so an override that does unconditional work
// cannot tell the two apart — only the event's property names and change type can.
USTRUCT()
struct FTestPropertyChangeEventNested
{
    GENERATED_BODY()

    UPROPERTY()
    int32 InnerScalar = 0;
};

UCLASS()
class UTestPropertyChangeEventHost : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    int32 Scalar = 0;

    UPROPERTY()
    TArray<int32> Numbers;

    UPROPERTY()
    FTestPropertyChangeEventNested Nested;

    // Recorded shape of the last change event delivered to this object. Plain fields,
    // not UPROPERTYs: they are test instrumentation, never serialized state.
    FName LastPropertyName = NAME_None;
    FName LastMemberPropertyName = NAME_None;
    EPropertyChangeType::Type LastChangeType = EPropertyChangeType::Unspecified;
    int32 NotifyCount = 0;

    void ResetChangeRecord()
    {
        LastPropertyName = NAME_None;
        LastMemberPropertyName = NAME_None;
        LastChangeType = EPropertyChangeType::Unspecified;
        NotifyCount = 0;
    }

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
    {
        LastPropertyName = PropertyChangedEvent.GetPropertyName();
        LastMemberPropertyName = PropertyChangedEvent.GetMemberPropertyName();
        LastChangeType = PropertyChangedEvent.ChangeType;
        ++NotifyCount;
        Super::PostEditChangeProperty(PropertyChangedEvent);
    }
#endif
};
