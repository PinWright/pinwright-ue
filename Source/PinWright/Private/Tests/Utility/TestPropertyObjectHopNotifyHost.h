// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "TestPropertyObjectHopNotifyHost.generated.h"

// Fixture types for TestPropertyObjectHopNotify.cpp (B-property-set-object-hop-notification-noop).
//
// The defect is NOT observable from the value: a path that hops through an
// FObjectProperty stores into the inner object correctly, and reading it back agrees.
// What never happened is the inner object's PostEditChangeProperty. So the inner class
// below carries DERIVED state that only its override recomputes, name-matched exactly the
// way engine overrides are written - the same shape as UPCGSettings::PostEditChangeProperty,
// where the settings object's broadcast is the only route to a regenerated graph. A write
// through the hop that leaves DerivedFromSource stale is the defect; the value landing is
// not evidence of anything.
USTRUCT()
struct FTestObjectHopNotifyNested
{
    GENERATED_BODY()

    UPROPERTY()
    int32 InnerScalar = 0;
};

UCLASS()
class UTestObjectHopNotifyInner : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    int32 Source = 0;

    UPROPERTY()
    TArray<int32> SourceNumbers;

    UPROPERTY()
    FTestObjectHopNotifyNested Nested;

    // Derived state plus the recorded event shape. Plain fields, not UPROPERTYs: test
    // instrumentation, never serialized state. The sentinels are deliberately values no
    // recompute below can produce, so "never ran" and "ran and computed 0" stay distinct.
    int32 DerivedFromSource = -1;
    int32 DerivedFromNumbers = -1;
    int32 DerivedFromNested = -1;
    FName LastPropertyName = NAME_None;
    FName LastMemberPropertyName = NAME_None;
    EPropertyChangeType::Type LastChangeType = EPropertyChangeType::Unspecified;
    int32 NotifyCount = 0;

    void ResetChangeRecord()
    {
        DerivedFromSource = -1;
        DerivedFromNumbers = -1;
        DerivedFromNested = -1;
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

        // Name-matched branches, exactly as engine overrides are written: an event that
        // reaches the wrong object cannot fire them, and neither can an empty one.
        if (LastPropertyName == GET_MEMBER_NAME_CHECKED(UTestObjectHopNotifyInner, Source))
        {
            DerivedFromSource = Source * 10;
        }
        else if (LastPropertyName == GET_MEMBER_NAME_CHECKED(UTestObjectHopNotifyInner, SourceNumbers))
        {
            DerivedFromNumbers = SourceNumbers.Num();
        }
        else if (LastMemberPropertyName == GET_MEMBER_NAME_CHECKED(UTestObjectHopNotifyInner, Nested))
        {
            // Member-matched, the half a nested write only reaches when the member is
            // resolved against THIS object rather than the object the path started from.
            DerivedFromNested = Nested.InnerScalar * 10;
        }

        Super::PostEditChangeProperty(PropertyChangedEvent);
    }
#endif
};

// The object a hopping path STARTS from. It records notifications only so a test can
// assert it received none: the event has to move to the inner object, not be duplicated.
UCLASS()
class UTestObjectHopNotifyOuter : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TObjectPtr<UTestObjectHopNotifyInner> Inner;

    UPROPERTY()
    TArray<TObjectPtr<UTestObjectHopNotifyInner>> InnerArray;

    // A struct member on the outer, for the case that must NOT change behaviour: a struct
    // hop keeps the value in this object's own memory, so this object stays the target.
    UPROPERTY()
    FTestObjectHopNotifyNested OuterNested;

    int32 DerivedFromOuterNested = -1;
    FName LastPropertyName = NAME_None;
    FName LastMemberPropertyName = NAME_None;
    int32 NotifyCount = 0;

    void ResetChangeRecord()
    {
        DerivedFromOuterNested = -1;
        LastPropertyName = NAME_None;
        LastMemberPropertyName = NAME_None;
        NotifyCount = 0;
    }

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
    {
        LastPropertyName = PropertyChangedEvent.GetPropertyName();
        LastMemberPropertyName = PropertyChangedEvent.GetMemberPropertyName();
        ++NotifyCount;

        if (LastMemberPropertyName == GET_MEMBER_NAME_CHECKED(UTestObjectHopNotifyOuter, OuterNested))
        {
            DerivedFromOuterNested = OuterNested.InnerScalar * 10;
        }

        Super::PostEditChangeProperty(PropertyChangedEvent);
    }
#endif
};
