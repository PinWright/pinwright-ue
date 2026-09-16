// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRGrammar.h"

namespace
{
FString NormalizeKeyword(const FString& Keyword)
{
    return Keyword.ToLower();
}
}

const FMGIRGrammar& FMGIRGrammar::Get()
{
    static const FMGIRGrammar Grammar;
    return Grammar;
}

bool FMGIRGrammar::IsKeyword(const FString& Word) const
{
    int32 IgnoredOpcode = 0;
    return TryGetOpcode(Word, IgnoredOpcode) || GetSyntaxKeywords().Contains(NormalizeKeyword(Word));
}

bool FMGIRGrammar::TryGetOpcode(const FString& Keyword, int32& OutOpcode) const
{
    if (const EMGIROpcode* Opcode = GetOpcodeKeywords().Find(NormalizeKeyword(Keyword)))
    {
        OutOpcode = static_cast<int32>(*Opcode);
        return true;
    }

    return false;
}

FString FMGIRGrammar::OpcodeToText(EMGIROpcode Opcode)
{
    switch (Opcode)
    {
    case EMGIROpcode::Call: return TEXT("call");
    case EMGIROpcode::Output: return TEXT("output");
    case EMGIROpcode::Reroute: return TEXT("reroute");
    case EMGIROpcode::FunctionCall: return TEXT("function_call");
    case EMGIROpcode::LayerStack: return TEXT("layer_stack");
    case EMGIROpcode::Constant: return TEXT("constant");
    case EMGIROpcode::Property: return TEXT("property");
    default: return FString();
    }
}

const TMap<FString, EMGIROpcode>& FMGIRGrammar::GetOpcodeKeywords()
{
    static const TMap<FString, EMGIROpcode> Keywords = {
        { TEXT("call"), EMGIROpcode::Call },
        { TEXT("output"), EMGIROpcode::Output },
        { TEXT("out"), EMGIROpcode::Output },
        { TEXT("reroute"), EMGIROpcode::Reroute },
        { TEXT("function_call"), EMGIROpcode::FunctionCall },
        { TEXT("function"), EMGIROpcode::FunctionCall },
        { TEXT("layer_stack"), EMGIROpcode::LayerStack },
        { TEXT("constant"), EMGIROpcode::Constant },
        { TEXT("const"), EMGIROpcode::Constant },
        { TEXT("property"), EMGIROpcode::Property },
    };
    return Keywords;
}

const TSet<FString>& FMGIRGrammar::GetSyntaxKeywords()
{
    static const TSet<FString> Keywords = {
        TEXT("entry"),
        TEXT("material"),
        TEXT("function"),
    };
    return Keywords;
}
