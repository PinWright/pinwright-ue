// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRGrammar.h"

namespace
{
FString NormalizeKeyword(const FString& Keyword)
{
    return Keyword.ToLower();
}
}

const FCRIRGrammar& FCRIRGrammar::Get()
{
    static const FCRIRGrammar Grammar;
    return Grammar;
}

bool FCRIRGrammar::IsKeyword(const FString& Word) const
{
    int32 IgnoredOpcode = 0;
    return TryGetOpcode(Word, IgnoredOpcode) || GetSyntaxKeywords().Contains(NormalizeKeyword(Word));
}

bool FCRIRGrammar::TryGetOpcode(const FString& Keyword, int32& OutOpcode) const
{
    if (const ECRIROpcode* Opcode = GetOpcodeKeywords().Find(NormalizeKeyword(Keyword)))
    {
        OutOpcode = static_cast<int32>(*Opcode);
        return true;
    }

    return false;
}

FString FCRIRGrammar::OpcodeToText(ECRIROpcode Opcode)
{
    switch (Opcode)
    {
    case ECRIROpcode::Unit:        return TEXT("unit");
    case ECRIROpcode::Var:         return TEXT("var");
    case ECRIROpcode::Reroute:     return TEXT("reroute");
    case ECRIROpcode::Comment:     return TEXT("comment");
    case ECRIROpcode::If:          return TEXT("if");
    case ECRIROpcode::Select:      return TEXT("select");
    case ECRIROpcode::Enum:        return TEXT("enum");
    case ECRIROpcode::InvokeEntry: return TEXT("invoke_entry");
    case ECRIROpcode::Template:    return TEXT("template");
    case ECRIROpcode::Dispatch:    return TEXT("dispatch");
    case ECRIROpcode::Collapse:        return TEXT("collapse");
    case ECRIROpcode::FunctionRef:     return TEXT("function_ref");
    case ECRIROpcode::FunctionEntry:   return TEXT("function_entry");
    case ECRIROpcode::FunctionReturn:  return TEXT("function_return");
    case ECRIROpcode::ExposedPin:      return TEXT("exposed_pin");
    default:                       return FString();
    }
}

const TMap<FString, ECRIROpcode>& FCRIRGrammar::GetOpcodeKeywords()
{
    static const TMap<FString, ECRIROpcode> Keywords = {
        { TEXT("unit"),         ECRIROpcode::Unit },
        { TEXT("var"),          ECRIROpcode::Var },
        { TEXT("reroute"),      ECRIROpcode::Reroute },
        { TEXT("comment"),      ECRIROpcode::Comment },
        { TEXT("if"),           ECRIROpcode::If },
        { TEXT("select"),       ECRIROpcode::Select },
        { TEXT("enum"),         ECRIROpcode::Enum },
        { TEXT("invoke_entry"), ECRIROpcode::InvokeEntry },
        { TEXT("template"),     ECRIROpcode::Template },
        { TEXT("dispatch"),     ECRIROpcode::Dispatch },
        { TEXT("collapse"),        ECRIROpcode::Collapse },
        { TEXT("function_ref"),    ECRIROpcode::FunctionRef },
        { TEXT("function_entry"),  ECRIROpcode::FunctionEntry },
        { TEXT("function_return"), ECRIROpcode::FunctionReturn },
        { TEXT("exposed_pin"),     ECRIROpcode::ExposedPin },
    };
    return Keywords;
}

const TSet<FString>& FCRIRGrammar::GetSyntaxKeywords()
{
    // Top-level block-opener keywords (`rig_graph` / `rig_hierarchy`) plus the
    // hierarchy element kinds. Element kinds aren't RigVM opcodes — they
    // only appear inside `rig_hierarchy` blocks — but the tokenizer needs them
    // classified as keywords so their leading positions parse cleanly.
    static const TSet<FString> Keywords = {
        TEXT("rig_graph"),
        TEXT("rig_hierarchy"),
        TEXT("rig_function"),
        TEXT("rig_subgraph"),
        TEXT("bone"),
        TEXT("null"),
        TEXT("control"),
        TEXT("socket"),
        TEXT("curve"),
    };
    return Keywords;
}
