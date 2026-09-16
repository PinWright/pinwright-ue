// Copyright (c) 2026 Alexander Penkin. MIT License.

// Test-only UCLASS used by TestBpirTargetClassPriority.cpp to reproduce the
// self-class/target-class name collision documented in
// B-bpir-target-shadowed-by-self-class. The method name intentionally collides
// with UUserWidget::SetPlaybackSpeed(UWidgetAnimation*, float) so the cascade
// reorder (Fix 1) and qualified-call syntax (Fix 2) can be exercised in
// isolation against a UUserWidget-parented test Blueprint.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TestBpirTargetClassPriorityFixture.generated.h"

UCLASS()
class UBpirTargetClassPriorityTestSubsystem : public UObject
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Test")
    void SetPlaybackSpeed(float Speed) {}
};
