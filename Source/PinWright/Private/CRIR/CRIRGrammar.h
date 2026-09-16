// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IIrGrammar.h"
#include "CRIR/CRIROpcodes.h"

class PINWRIGHT_API FCRIRGrammar final : public IIrGrammar
{
public:
    virtual bool IsKeyword(const FString& Word) const override;
    virtual bool TryGetOpcode(const FString& Keyword, int32& OutOpcode) const override;

    static const FCRIRGrammar& Get();
    static FString OpcodeToText(ECRIROpcode Opcode);

private:
    static const TMap<FString, ECRIROpcode>& GetOpcodeKeywords();
    static const TSet<FString>& GetSyntaxKeywords();
};

// Shared between the CRIR text emitter (block-opener keyword) and the CRIR
// decompiler (per-block header label). `rig_graph` accepts an optional name
// token; `rig_hierarchy` does not.
inline const TCHAR* EntryKindToText(ECRIREntryKind Kind)
{
    switch (Kind)
    {
    case ECRIREntryKind::RigGraph:     return TEXT("rig_graph");
    case ECRIREntryKind::RigHierarchy: return TEXT("rig_hierarchy");
    case ECRIREntryKind::RigFunction:  return TEXT("rig_function");
    default:                           return TEXT("rig_graph");
    }
}

inline const TCHAR* ElementKindToText(ECRIRElementKind Kind)
{
    switch (Kind)
    {
    case ECRIRElementKind::Bone:    return TEXT("bone");
    case ECRIRElementKind::Null:    return TEXT("null");
    case ECRIRElementKind::Control: return TEXT("control");
    case ECRIRElementKind::Socket:  return TEXT("socket");
    case ECRIRElementKind::Curve:   return TEXT("curve");
    default:                        return TEXT("bone");
    }
}
