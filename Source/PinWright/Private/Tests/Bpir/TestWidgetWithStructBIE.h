// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/EngineTypes.h"
#include "TestWidgetWithStructBIE.generated.h"

UCLASS()
class UTestWidgetWithStructBIE : public UUserWidget
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintImplementableEvent)
    void TestStructBIE(const FHitResult& InHit);
};
