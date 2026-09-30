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

    // Modifier list inside a key_pressed/key_released signature, in UK2Node_InputKey
    // declaration order; empty for a plain key so modifier-free BPIR stays unchanged.
    inline FString FormatInputKeyModifiers(const bool bCtrl, const bool bAlt, const bool bShift, const bool bCmd)
    {
        TArray<FString> Parts;
        if (bCtrl)  { Parts.Add(TEXT("ctrl")); }
        if (bAlt)   { Parts.Add(TEXT("alt")); }
        if (bShift) { Parts.Add(TEXT("shift")); }
        if (bCmd)   { Parts.Add(TEXT("cmd")); }
        return FString::Join(Parts, TEXT(", "));
    }

    // Parses the modifier list of a key entry ("ctrl, shift"), any order and case. Returns false
    // and the offending token in OutUnknown for anything but ctrl / alt / shift / cmd.
    inline bool ParseInputKeyModifiers(
        const FString& List, bool& bOutCtrl, bool& bOutAlt, bool& bOutShift, bool& bOutCmd, FString& OutUnknown)
    {
        TArray<FString> Modifiers;
        List.ParseIntoArray(Modifiers, TEXT(","));
        for (const FString& RawModifier : Modifiers)
        {
            const FString Modifier = RawModifier.TrimStartAndEnd();
            bool* Flag = Modifier.Equals(TEXT("ctrl"), ESearchCase::IgnoreCase) ? &bOutCtrl
                : Modifier.Equals(TEXT("alt"), ESearchCase::IgnoreCase) ? &bOutAlt
                : Modifier.Equals(TEXT("shift"), ESearchCase::IgnoreCase) ? &bOutShift
                : Modifier.Equals(TEXT("cmd"), ESearchCase::IgnoreCase) ? &bOutCmd
                : nullptr;
            if (!Flag)
            {
                OutUnknown = Modifier;
                return false;
            }
            *Flag = true;
        }
        return true;
    }

    inline FString FormatInputKeyModifiers(const UK2Node_InputKey* InputKeyNode)
    {
        return FormatInputKeyModifiers(
            InputKeyNode->bControl, InputKeyNode->bAlt, InputKeyNode->bShift, InputKeyNode->bCommand);
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

    // Key plus modifiers: `J` and `J(ctrl)` are different keys. Modifiers is the
    // FormatInputKeyModifiers form, empty for a plain key.
    inline bool DoesInputKeyMatchBpirKey(
        UK2Node_InputKey* InputKeyNode,
        const FString& KeyIdentifier,
        const FString& Modifiers)
    {
        return DoesInputKeyMatchBpirIdentifier(InputKeyNode, KeyIdentifier)
            && FormatInputKeyModifiers(InputKeyNode) == Modifiers;
    }

    inline bool IsInputKeyNodeForBpirEntry(
        UK2Node_InputKey* InputKeyNode,
        const FString& KeyIdentifier,
        const FString& Modifiers,
        const bool bReleased)
    {
        return DoesInputKeyMatchBpirKey(InputKeyNode, KeyIdentifier, Modifiers)
            && IsInputKeyExecPinActive(InputKeyNode, bReleased);
    }
}
