// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirParser.h - Line-based parser for BPIR (Blueprint IR) text

#pragma once

#include "CoreMinimal.h"
#include "Compiler/BpirTypes.h"
#include "IrCore/IrToken.h"

struct FCompileError;

class FBpirParser;

// Signature for dispatch-table handler functions
using FInstructionParserFn = TFunction<void(FBpirParser&, const FString& Rest, const FString& ResultName)>;

// Parses BPIR text into structured entry blocks with instructions.
// Pure text processing — no UE graph/node dependencies.
class PINWRIGHT_API FBpirParser
{
public:
    // Parse BPIR code into entry blocks. Returns true if no errors.
    bool Parse(const FString& Code, TArray<FBpirEntryBlock>& OutBlocks, TArray<FCompileError>& OutErrors, bool bSkipReferenceValidation = false);

    // Parse a headless body (no entry wrapper) for InsertCodeAfterNode mode.
    bool ParseBody(const FString& Code, FBpirEntryBlock& OutBlock, TArray<FCompileError>& OutErrors);

private:
    // Entry signature parsing
    bool ParseEntryLine(const FString& Line, int32 LineNum, FBpirEntryBlock& OutBlock, TArray<FCompileError>& OutErrors);

    // @meta(Key=Value, ...) decorator line above an `entry …` signature.
    // OutMeta is updated in place. Returns false on syntax / whitelist failure.
    bool ParseMetaDecorator(const FString& Line, int32 LineNum, FBpirEntryMetadata& OutMeta, TArray<FCompileError>& OutErrors);

    // @flags(Identifier, ...) decorator line above an `entry …` signature.
    // OutMeta is updated in place. Returns false on syntax / whitelist failure.
    bool ParseFlagsDecorator(const FString& Line, int32 LineNum, FBpirEntryMetadata& OutMeta, TArray<FCompileError>& OutErrors);

    // Reject @flags on macros / event entries and both decorators on event entries.
    // Runs after ParseEntryLine has populated OutBlock.Kind.
    void ValidateMetadataKindRestrictions(const FBpirEntryBlock& Block, int32 LineNum, TArray<FCompileError>& OutErrors);

    // Instruction parsing by first-token dispatch
    bool ParseInstruction(const FString& Line, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);

    // Sub-parsers
    bool ParseCallInstruction(const FString& Line, int32 LineNum, EBpirOpcode Opcode, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseBranchInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseForeachInstruction(const FString& Rest, int32 LineNum, bool bWithBreak, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseWhileInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseSwitchInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseSwitchIntInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseSwitchStringInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseSwitchEnumInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseSequenceInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseCastInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseSelectInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseMacroInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseLatentInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseTimelineInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseStructInstruction(const FString& Rest, int32 LineNum, bool bMake, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseDispatcherInstruction(const FString& Rest, int32 LineNum, EBpirOpcode Opcode, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseClearDispatcherInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);
    bool ParseSubsystemInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);

    // Argument and exec clause parsing
    TArray<FBpirArg> ParseArgs(const FString& ArgsStr);
    TArray<FBpirExecTarget> ParseExecClause(const FString& ClauseStr, int32 LineNum = 0, TArray<FCompileError>* OutErrors = nullptr);

    // Parse exec clause from suffix string "[...] after parens". Reports error on unmatched bracket.
    bool ParseExecClauseFromSuffix(const FString& AfterParens, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors);

    // Build label/value index tables and validate references
    bool BuildIndicesAndValidate(FBpirEntryBlock& Block, TArray<FCompileError>& OutErrors, bool bSkipReferenceValidation = false);

    // Pin name normalization: "Array Element" -> "ArrayElement"
    static FString NormalizePinName(const FString& Name);

    static bool IsBpirInstructionKeyword(const FString& Keyword);

    static bool TryExtractAuthoredPosition(FString& InOutLine, FVector2D& OutPosition, FString& OutError);

    // Extract content between balanced parentheses starting at a given position
    static int32 FindMatchingParen(const FString& Str, int32 OpenPos);

    // Extract content between balanced brackets
    static int32 FindMatchingBracket(const FString& Str, int32 OpenPos);

    // Extract content between balanced braces
    static int32 FindMatchingBrace(const FString& Str, int32 OpenPos);

    // Extract content between balanced angle brackets
    static int32 FindMatchingAngle(const FString& Str, int32 OpenPos);

    // TMap dispatch tables for ParseInstruction
    static const TMap<FString, FInstructionParserFn>& GetTopLevelDispatch();
    static const TMap<FString, FInstructionParserFn>& GetAfterEqualsDispatch();

    // Transient state stashed during dispatch (valid only inside ParseInstruction)
    int32 CurrentLineNum = 0;
    FBpirInstruction* CurrentInstPtr = nullptr;
    TArray<FCompileError>* CurrentErrorsPtr = nullptr;

    FBpirInstruction& CurrentInst() { return *CurrentInstPtr; }
    TArray<FCompileError>& CurrentErrors() { return *CurrentErrorsPtr; }
};
