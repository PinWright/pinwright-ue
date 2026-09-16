// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirSharedConstants.h - Cross-file load-bearing string literals shared between BPIR
// compiler-parse and decompiler-emit paths. Keeping these in one header prevents silent
// drift when one side renames a literal and the round-trip breaks.

#pragma once
#include "Containers/UnrealString.h"

namespace BpirSharedConstants
{
    namespace AsyncAction
    {
        inline constexpr const TCHAR ClassPrefix[] = TEXT("K2Node_AsyncAction_");
        // Length excluding null terminator — for RightChop/Mid math at call sites.
        inline constexpr int32 ClassPrefixLen = UE_ARRAY_COUNT(ClassPrefix) - 1;
    }
    namespace MacroNames
    {
        inline const TCHAR* const While = TEXT("WhileLoop");
        inline const TCHAR* const ForEachLoop = TEXT("ForEachLoop");
        inline const TCHAR* const ForEachLoopWithBreak = TEXT("ForEachLoopWithBreak");
    }
    namespace FieldNotify
    {
        inline const TCHAR* const SubscribeFnName = TEXT("K2_AddFieldValueChangedDelegate");
        inline const TCHAR* const UnsubscribeFnName = TEXT("K2_RemoveFieldValueChangedDelegate");
    }
    namespace Keywords
    {
        inline const TCHAR* const NodeProps = TEXT("node_props");
        // Optional BPIR node-state suffixes. They are parsed as bare tokens at
        // the end of an instruction or entry signature, immediately before an
        // authored-position marker when one is present.
        inline const TCHAR* const Enabled = TEXT("enabled");
        inline const TCHAR* const Disabled = TEXT("disabled");
        inline const TCHAR* const DevelopmentOnly = TEXT("devonly");
        // Instruction keywords the decompiler emits and the parser dispatches on.
        // `Message` is the interface-message form of `Call` (UK2Node_Message).
        inline const TCHAR* const Call = TEXT("call");
        inline const TCHAR* const Message = TEXT("message");
        // `ParentCall` preserves UK2Node_CallParentFunction in a round trip.
        inline const TCHAR* const ParentCall = TEXT("parent_call");
        inline const TCHAR* const End = TEXT("end");
    }
    namespace Syntax
    {
        // Class/method qualifier in BPIR call syntax: `ClassName::MethodName(...)`.
        inline const TCHAR* const QualifiedNameSeparator = TEXT("::");
    }
    namespace EnhancedInput
    {
        inline constexpr const TCHAR* EventPinNames[] = {
            TEXT("Triggered"), TEXT("Started"), TEXT("Ongoing"),
            TEXT("Canceled"), TEXT("Completed")
        };

        inline bool IsEventPinName(const FString& Name)
        {
            for (const TCHAR* EventPinName : EventPinNames)
            {
                if (Name == EventPinName)
                {
                    return true;
                }
            }
            return false;
        }
    }
}
