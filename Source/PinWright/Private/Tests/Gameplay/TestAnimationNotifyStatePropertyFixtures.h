// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "UObject/Object.h"
#include "TestAnimationNotifyStatePropertyFixtures.generated.h"

UCLASS()
class UTestAnimationNotifyStatePayload : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    FName WarpTargetName;
};

UCLASS()
class UTestAnimationNotifyStateWithPayload : public UAnimNotifyState
{
    GENERATED_BODY()

public:
    UPROPERTY(Instanced)
    TObjectPtr<UTestAnimationNotifyStatePayload> Payload;
};
