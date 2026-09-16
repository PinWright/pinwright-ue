// Copyright (c) 2026 Alexander Penkin. MIT License.

// K2Node_BpirExpression.h - Custom blueprint node that compiles BPIR text into a collapsed sub-graph

#pragma once

#include "CoreMinimal.h"
#include "K2Node_Composite.h"
#include "K2Node_BpirExpression.generated.h"

UCLASS()
class PINWRIGHT_API UK2Node_BpirExpression : public UK2Node_Composite
{
    GENERATED_BODY()

public:
    UK2Node_BpirExpression();

    UPROPERTY(EditAnywhere, Category="BPIR", meta=(MultiLine="true"))
    FString BpirText;

    // --- UEdGraphNode overrides ---
    virtual void PostPlacedNewNode() override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
    virtual FText GetTooltipText() const override;
    virtual void ReconstructNode() override;
    virtual void PostEditUndo() override;

    // --- UK2Node overrides ---
    virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
    virtual FText GetMenuCategory() const override;
    virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;
    virtual FLinearColor GetNodeTitleColor() const override;
    virtual bool IsNodePure() const override;

    // --- UK2Node_EditablePinBase override ---
    virtual bool CanCreateUserDefinedPin(const FEdGraphPinType& InPinType, EEdGraphPinDirection InDesiredDirection, FText& OutErrorMessage) override;

    // Rebuild the sub-graph from BpirText
    void RebuildFromBpir();

private:
    void ClearSubgraph();

    TArray<FString> CachedErrors;

    mutable FNodeTextCache CachedNodeTitle;

    static bool bIsRebuilding;
};
