// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRCompilerHelpers.h
//
// Shared helpers extracted from AGIRCompiler.cpp and the per-family
// AGIRCompiler_*.cpp translation units. Previously these helpers lived in
// per-file anonymous namespaces; under unity builds (UBT merges 8+ .cpp files
// into one TU) duplicate definitions tripped C2084. Consolidating into a
// single named namespace gives one definition per program.

#pragma once

#include "CoreMinimal.h"
#include "AGIR/AGIROpcodes.h"
#include "EdGraph/EdGraphNode.h"

class UAnimGraphNode_Base;
class UObject;

namespace AGIRCliff
{
namespace Helpers
{
// Detects a pose-link arg by value shape: `%nNN` or `%name`. Reflective field
// args are written via WriteAnimNodeArg; pose args are queued for Pass 2.
inline bool IsPoseRefValue(const FString& Value)
{
    return !Value.IsEmpty() && Value[0] == TEXT('%');
}

// Set NodeGuid from the AGIR annotation if present. NodeCreator already called
// CreateNewGuid(); overwriting after Finalize is allowed and round-trips
// cross-graph pointer fields like K2Node_TransitionRuleGetter.
inline void ApplyNodeGuidIfPresent(UEdGraphNode* Node, const FString& GuidText)
{
    if (!Node || GuidText.IsEmpty())
    {
        return;
    }
    FGuid Parsed;
    if (FGuid::Parse(GuidText, Parsed))
    {
        Node->NodeGuid = Parsed;
    }
}

// Look up an arg by name (case-sensitive) in an AGIR instruction. Returns a
// pointer to the value string or nullptr when absent.
inline const FString* FindArgValue(const FAGIRInstruction& Inst, const TCHAR* ArgName)
{
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name == ArgName)
        {
            return &Arg.Value;
        }
    }
    return nullptr;
}

// Reflective write onto a UPROPERTY of the target UObject. Used for editor
// fields that live directly on the editor object (e.g. PriorityOrder,
// CrossfadeDuration, NameOfCache) rather than on a runtime FAnimNode_* struct.
// Returns an empty string on success, or an error description on failure.
FString WriteUObjectFieldByName(UObject* Target, FName FieldName, const FString& ValueAsText);

// The single write path for one non-pose AGIR field-list arg on an anim node.
// Data-pin binding values (`$Variable`, `bind <path>` — see AGIRPinBindings.h)
// create the binding they name; everything else is a literal and falls through
// to the reflective struct write. Every per-family compile handler routes its
// args through here so a binding is honoured wherever it appears, instead of
// being fed to ImportText and dropped. Returns an empty string on success.
FString WriteAnimNodeArg(UAnimGraphNode_Base* Node, FName FieldName, const FString& ValueAsText);
} // namespace Helpers
} // namespace AGIRCliff
