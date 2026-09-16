// Copyright (c) 2026 Alexander Penkin. MIT License.

// K2Node_BpirExpression.cpp - BPIR expression node implementation

#include "K2Node_BpirExpression.h"
#include "Compiler/BpirSubgraphCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Tunnel.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"

#define LOCTEXT_NAMESPACE "K2Node_BpirExpression"

bool UK2Node_BpirExpression::bIsRebuilding = false;

UK2Node_BpirExpression::UK2Node_BpirExpression()
{
    bCanRenameNode = false;
}

void UK2Node_BpirExpression::PostPlacedNewNode()
{
    Super::PostPlacedNewNode();

    if (BoundGraph)
    {
        const FName UniqueName = MakeUniqueObjectName(GetOuter(), UEdGraph::StaticClass(), TEXT("BpirExpressionGraph"));
        BoundGraph->Rename(*UniqueName.ToString(), GetOuter());
    }
}

void UK2Node_BpirExpression::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UK2Node_BpirExpression, BpirText))
    {
        RebuildFromBpir();
    }
}

FText UK2Node_BpirExpression::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
    if (TitleType == ENodeTitleType::FullTitle)
    {
        if (CachedNodeTitle.IsOutOfDate(this))
        {
            FString TitleLine;
            if (BpirText.TrimStartAndEnd().IsEmpty())
            {
                TitleLine = TEXT("BPIR Expression (empty)");
            }
            else
            {
                // Find first non-comment, non-blank, non-declaration line
                TArray<FString> Lines;
                BpirText.ParseIntoArrayLines(Lines);
                bool bPastHeader = false;
                for (const FString& Line : Lines)
                {
                    FString Trimmed = Line.TrimStartAndEnd();
                    if (Trimmed == TEXT("---"))
                    {
                        bPastHeader = true;
                        continue;
                    }
                    if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT("//")))
                    {
                        continue;
                    }
                    // Skip declaration lines (before ---)
                    if (!bPastHeader && (Trimmed.StartsWith(TEXT("in ")) || Trimmed.StartsWith(TEXT("out "))))
                    {
                        continue;
                    }
                    TitleLine = Trimmed;
                    break;
                }
                if (TitleLine.IsEmpty())
                {
                    TitleLine = TEXT("BPIR Expression");
                }
                else if (TitleLine.Len() > 40)
                {
                    TitleLine = TitleLine.Left(37) + TEXT("...");
                }
            }

            FFormatNamedArguments Args;
            Args.Add(TEXT("FirstLine"), FText::FromString(TitleLine));
            CachedNodeTitle.SetCachedText(
                FText::Format(LOCTEXT("BpirFullTitle", "{FirstLine}\nBPIR Expression"), Args), this);
        }
        return CachedNodeTitle;
    }

    if (BpirText.TrimStartAndEnd().IsEmpty())
    {
        return LOCTEXT("BpirTitleEmpty", "BPIR Expression (empty)");
    }
    return LOCTEXT("BpirTitle", "BPIR Expression");
}

FText UK2Node_BpirExpression::GetTooltipText() const
{
    return LOCTEXT("BpirTooltip", "BPIR Expression Node\nCompiles BPIR text into a collapsed Blueprint sub-graph.");
}

void UK2Node_BpirExpression::ReconstructNode()
{
    RebuildFromBpir();
    // Save error state — UK2Node::ReconstructNode() clears ErrorMsg
    const FString SavedErrorMsg = ErrorMsg;
    Super::ReconstructNode();
    ErrorMsg = SavedErrorMsg;
}

void UK2Node_BpirExpression::PostEditUndo()
{
    Super::PostEditUndo();
    RebuildFromBpir();
}

void UK2Node_BpirExpression::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
    if (!ActionRegistrar.IsOpenForRegistration(GetClass()))
    {
        return;
    }

    UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(GetClass());
    check(Spawner);
    ActionRegistrar.AddBlueprintAction(GetClass(), Spawner);
}

FText UK2Node_BpirExpression::GetMenuCategory() const
{
    return LOCTEXT("BpirExpressionCategory", "BPIR");
}

void UK2Node_BpirExpression::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
    Super::ValidateNodeDuringCompilation(MessageLog);

    for (const FString& Error : CachedErrors)
    {
        MessageLog.Error(*FString::Printf(TEXT("%s"), *Error), this);
    }
}

FLinearColor UK2Node_BpirExpression::GetNodeTitleColor() const
{
    return FLinearColor(0.0f, 0.6f, 0.6f);
}

bool UK2Node_BpirExpression::IsNodePure() const
{
    for (const UEdGraphPin* Pin : Pins)
    {
        if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return false;
        }
    }
    return true;
}

bool UK2Node_BpirExpression::CanCreateUserDefinedPin(const FEdGraphPinType& InPinType, EEdGraphPinDirection InDesiredDirection, FText& OutErrorMessage)
{
    OutErrorMessage = LOCTEXT("PinsManagedByBpir", "Pins are managed by the BPIR text");
    return false;
}

void UK2Node_BpirExpression::RebuildFromBpir()
{
    if (bIsRebuilding)
    {
        return;
    }

    bIsRebuilding = true;
    ON_SCOPE_EXIT { bIsRebuilding = false; };

    ClearSubgraph();
    CachedErrors.Empty();
    CachedNodeTitle.MarkDirty();

    if (BpirText.TrimStartAndEnd().IsEmpty())
    {
        ErrorMsg.Empty();
        bHasCompilerMessage = false;
        return;
    }

    UBlueprint* Blueprint = GetBlueprint();
    if (!Blueprint)
    {
        return;
    }

    if (!BoundGraph || !InputSinkNode || !OutputSourceNode)
    {
        CachedErrors.Add(TEXT("BPIR Expression: Missing bound graph or tunnel nodes"));
        bHasCompilerMessage = true;
        ErrorType = EMessageSeverity::Error;
        ErrorMsg = CachedErrors[0];
        return;
    }

    UK2Node_Tunnel* Entry = GetEntryNode();
    UK2Node_Tunnel* Exit = GetExitNode();
    FBpirSubgraphCompiler Compiler(Blueprint, BoundGraph, Entry, Exit);
    FCompileResult Result = Compiler.CompileIntoSubgraph(BpirText);

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            CachedErrors.Add(Err.Message);
        }

        bHasCompilerMessage = true;
        ErrorType = EMessageSeverity::Error;
        ErrorMsg = Result.Errors.Num() > 0 ? Result.Errors[0].Message : TEXT("Unknown BPIR compilation error");
    }
    else
    {
        ErrorMsg.Empty();
        bHasCompilerMessage = false;
    }

    BoundGraph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
}

void UK2Node_BpirExpression::ClearSubgraph()
{
    if (!BoundGraph)
    {
        return;
    }

    // Remove all non-tunnel nodes
    TArray<UEdGraphNode*> NodesToRemove;
    for (UEdGraphNode* Node : BoundGraph->Nodes)
    {
        if (Node && Node->GetClass() != UK2Node_Tunnel::StaticClass())
        {
            NodesToRemove.Add(Node);
        }
    }
    for (UEdGraphNode* Node : NodesToRemove)
    {
        Node->DestroyNode();
    }

    // Clear user-defined pins on entry tunnel (use InputSinkNode directly to avoid check() assert)
    if (InputSinkNode)
    {
        while (InputSinkNode->UserDefinedPins.Num() > 0)
        {
            InputSinkNode->RemoveUserDefinedPin(InputSinkNode->UserDefinedPins.Last());
        }
    }

    // Clear user-defined pins on exit tunnel
    if (OutputSourceNode)
    {
        while (OutputSourceNode->UserDefinedPins.Num() > 0)
        {
            OutputSourceNode->RemoveUserDefinedPin(OutputSourceNode->UserDefinedPins.Last());
        }
    }
}

#undef LOCTEXT_NAMESPACE
