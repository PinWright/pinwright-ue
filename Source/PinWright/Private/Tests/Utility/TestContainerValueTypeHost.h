// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestContainerValueTypeHost.generated.h"

// Reflected enum used as a non-primitive map/array value type in the fixture below.
UENUM()
enum class ETestContainerValueGrade : uint8
{
    Bronze,
    Silver,
    Gold
};

// A small reflected struct standing in for the reported repro's FIKRetargetPose:
// a struct-typed map value that container.map.get/set must (de)serialize as a
// structured JSON object instead of rejecting with UNSUPPORTED_VALUE_TYPE.
USTRUCT()
struct FTestContainerPose
{
    GENERATED_BODY()

    UPROPERTY()
    FString Label;

    UPROPERTY()
    int32 Weight = 0;
};

// Fixture UCLASS for TestContainerValueTypeCoverage.cpp. Hosts maps and arrays whose
// VALUE/ELEMENT types are NON-primitive — struct, FName, FText, enum — none of which
// the old four-type (FStr/FInt/FFloat/FBool) CastField chain in the container.map.* /
// container.array.* handlers could read or write (F-container-map-value-type-coverage).
// Mirrors the live repro target (TMap<FName, FIKRetargetPose> on RTG_UE4Manny_UE5Manny):
// an FName-keyed, struct-valued map. Kept on a transient UObject so no engine CDO or
// on-disk asset is mutated.
UCLASS()
class UTestContainerValueTypeHost : public UObject
{
    GENERATED_BODY()

public:
    // FName -> struct: the exact key/value shape of the reported repro asset.
    UPROPERTY()
    TMap<FName, FTestContainerPose> NameToPose;

    // FString -> enum: a non-primitive enum value type.
    UPROPERTY()
    TMap<FString, ETestContainerValueGrade> NameToGrade;

    // Array of struct elements: the array-side analogue of the map value gap.
    UPROPERTY()
    TArray<FTestContainerPose> PoseList;

    // Array of FName elements: a non-primitive (non-FStr) array element type.
    UPROPERTY()
    TArray<FName> NameList;
};
