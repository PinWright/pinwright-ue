// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Decompiler/DecompilerTypes.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
// Case table for switch node analysis
struct FSwitchInfo
{
    TArray<TPair<FString, UEdGraphPin*>> Cases; // label->exec pin
    UEdGraphPin* DefaultPin = nullptr;
};

class PINWRIGHT_API FGraphWalker
{
public:
    explicit FGraphWalker(UEdGraph* InGraph);

    TArray<UEdGraphNode*> FindEntryPoints();
    ENodeSemantics ClassifyNode(UEdGraphNode* Node);
    bool IsLatentNode(UEdGraphNode* Node);

    UEdGraphPin* GetExecOutputPin(UEdGraphNode* Node, int32 Index = 0);
    TArray<UEdGraphPin*> GetAllExecOutputPins(UEdGraphNode* Node);
    UEdGraphPin* GetCompletionPin(UEdGraphNode* Node);

    FSwitchInfo AnalyzeSwitch(UEdGraphNode* SwitchNode);

private:
    FString GetMacroName(UEdGraphNode* MacroNode);

    UEdGraph* Graph;
};
