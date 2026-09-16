// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IIrGrammar.h"
#include "MGIR/MGIROpcodes.h"

class FMGIRGrammar final : public IIrGrammar
{
public:
    virtual bool IsKeyword(const FString& Word) const override;
    virtual bool TryGetOpcode(const FString& Keyword, int32& OutOpcode) const override;

    static const FMGIRGrammar& Get();
    static FString OpcodeToText(EMGIROpcode Opcode);

private:
    static const TMap<FString, EMGIROpcode>& GetOpcodeKeywords();
    static const TSet<FString>& GetSyntaxKeywords();
};
