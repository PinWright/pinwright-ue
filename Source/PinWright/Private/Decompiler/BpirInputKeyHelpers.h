// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirInputKeyHelpers.h - Shared FKey identifier and exec-sense helpers for BPIR InputKey entries.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "InputCoreTypes.h"
#include "K2Node_InputKey.h"

namespace FBpirInputKeyHelpers
{
    // Entry-signature and wire-literal emits must produce the same identifier — extract once, call from both.
    inline FString FormatInputKeyAsBpirIdentifier(const FKey& Key)
    {
        FString KeyName = Key.GetDisplayName().ToString();
        KeyName.ReplaceInline(TEXT(" "), TEXT(""));
        return KeyName;
    }

    inline FName GetInputKeyExecPinName(const bool bReleased)
    {
        return bReleased ? FName(TEXT("Released")) : FName(TEXT("Pressed"));
    }

    inline UEdGraphPin* FindInputKeyExecPin(UK2Node_InputKey* InputKeyNode, const bool bReleased)
    {
        if (!InputKeyNode)
        {
            return nullptr;
        }
        return InputKeyNode->FindPin(GetInputKeyExecPinName(bReleased), EGPD_Output);
    }

    inline bool IsInputKeyExecPinActive(UK2Node_InputKey* InputKeyNode, const bool bReleased)
    {
        UEdGraphPin* ExecPin = FindInputKeyExecPin(InputKeyNode, bReleased);
        return ExecPin
            && ExecPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            && ExecPin->LinkedTo.Num() > 0;
    }

    inline bool DoesInputKeyMatchBpirIdentifier(UK2Node_InputKey* InputKeyNode, const FString& KeyIdentifier)
    {
        return InputKeyNode
            && FormatInputKeyAsBpirIdentifier(InputKeyNode->InputKey).Equals(KeyIdentifier, ESearchCase::IgnoreCase);
    }

    inline bool IsInputKeyNodeForBpirEntry(
        UK2Node_InputKey* InputKeyNode,
        const FString& KeyIdentifier,
        const bool bReleased)
    {
        return DoesInputKeyMatchBpirIdentifier(InputKeyNode, KeyIdentifier)
            && IsInputKeyExecPinActive(InputKeyNode, bReleased);
    }
}
