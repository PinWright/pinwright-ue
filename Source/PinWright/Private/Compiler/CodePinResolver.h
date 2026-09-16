// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Compiler/BpirTypeSpec.h"

class UEdGraphPin;
struct FEdGraphPinType;

class PINWRIGHT_API FCodePinResolver
{
public:
    FCodePinResolver();
    void RegisterVariable(const FString& Name, UEdGraphPin* Pin);
    void RegisterLiteral(const FString& Name, const FString& Value);
    UEdGraphPin* ResolveVariable(const FString& Name) const;
    bool IsLiteral(const FString& Name) const;
    FString GetLiteralValue(const FString& Name) const;
    bool HasVariable(const FString& Name) const;
    void Clear();
    static bool SetPinDefaultValue(UEdGraphPin* Pin, const FString& Value, FString* OutError = nullptr);
    // Convert a parsed BPIR type spec into a pin type. Callers parse raw text
    // via BpirTypeSpecParser::ParseTypeSpec (or MakePinTypeFromBpirText) first.
    static bool ConvertTypeSpecToPinType(const FBpirTypeSpec& Spec, FEdGraphPinType& OutType);

private:
    TMap<FString, UEdGraphPin*> PinMap;
    TMap<FString, FString> LiteralMap;
};
