// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Editor-only reflected test fixture: a native UUserWidget subclass declaring one
// REQUIRED meta=(BindWidget) property and one OPTIONAL meta=(BindWidgetOptional)
// property. A UWidgetBlueprint parented to this class must satisfy RequiredBindText
// by variable name, so removing/renaming a widget named "RequiredBindText" breaks
// the required bind (the UMG compiler fails with "A required widget binding ... was
// not found."), while a widget named "OptionalBindText" never does. Used by
// TestWidgetRequiredBindWarning.cpp to drive widget.remove_widget / widget.rename_widget
// required-BindWidget advisory detection against the real production handlers.

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "TestWidgetRequiredBindFixture.generated.h"

UCLASS()
class UTestWidgetWithRequiredBind : public UUserWidget
{
    GENERATED_BODY()

public:
    UPROPERTY(meta = (BindWidget))
    TObjectPtr<UTextBlock> RequiredBindText;

    UPROPERTY(meta = (BindWidgetOptional))
    TObjectPtr<UTextBlock> OptionalBindText;
};
