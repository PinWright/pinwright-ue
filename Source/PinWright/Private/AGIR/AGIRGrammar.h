// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IIrGrammar.h"
#include "AGIR/AGIROpcodes.h"

class PINWRIGHT_API FAGIRGrammar final : public IIrGrammar
{
public:
    virtual bool IsKeyword(const FString& Word) const override;
    virtual bool TryGetOpcode(const FString& Keyword, int32& OutOpcode) const override;

    static const FAGIRGrammar& Get();
    static FString OpcodeToText(EAGIROpcode Opcode);

private:
    static const TMap<FString, EAGIROpcode>& GetOpcodeKeywords();
    static const TSet<FString>& GetSyntaxKeywords();
};

// Shared between the AGIR text emitter (entry block keyword) and the AGIR
// decompiler (per-graph header label). Returns the same token set
// (`anim_graph` / `anim_layer` / `anim_function`) so both call sites stay in
// lock-step on the entry-kind vocabulary.
inline const TCHAR* EntryKindToText(EAGIREntryKind Kind)
{
    switch (Kind)
    {
    case EAGIREntryKind::AnimGraph:         return TEXT("anim_graph");
    case EAGIREntryKind::AnimLayer:         return TEXT("anim_layer");
    case EAGIREntryKind::AnimFunction:      return TEXT("anim_function");
    case EAGIREntryKind::InterfaceManifest: return TEXT("interfaces");
    default:                                return TEXT("anim_graph");
    }
}
