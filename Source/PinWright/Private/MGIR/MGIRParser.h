// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "MGIR/MGIROpcodes.h"

class PINWRIGHT_API FMGIRParser
{
public:
    bool Parse(const FString& Code, TArray<FMGIREntryBlock>& OutBlocks, TArray<FMGIRParseError>& OutErrors);

private:
    bool ParseEntryLine(const FString& Line, int32 LineNum, FMGIREntryBlock& OutBlock, TArray<FMGIRParseError>& OutErrors) const;
    bool ParseInstruction(const FString& Line, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const;
    bool ParseAssignedInstruction(const FString& ResultName, const FString& Rest, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const;
    bool ParseOutputInstruction(const FString& Rest, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const;
    bool ParsePropertyInstruction(const FString& Rest, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const;
    bool BuildIndices(FMGIREntryBlock& Block, TArray<FMGIRParseError>& OutErrors) const;

    TArray<FMGIRArg> ParseArgs(const FString& ArgsText) const;
};
