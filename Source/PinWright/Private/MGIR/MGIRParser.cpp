// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRParser.h"

#include "IrCore/IrTextUtils.h"
#include "IrCore/IrTokenizer.h"
#include "MGIR/MGIRGrammar.h"
#include "MGIR/MGIRSubstrateSugar.h"
#include "MGIR/MGIRTypeGrammar.h"

namespace
{
void AddError(TArray<FMGIRParseError>& Errors, int32 Line, const FString& Message, const FString& Code = FString())
{
    Errors.Add(FMGIRParseError(Line, Message, Code));
}

bool IsUnescapedQuote(const FString& Str, int32 Index)
{
    return FIrTextUtils::IsUnescapedQuote(Str, Index);
}

FString StripTrailingComment(const FString& Value)
{
    return FIrTextUtils::StripTrailingComment(Value);
}

TArray<int32> FindTopLevelDelimiterPositions(const FString& Str, TCHAR Delimiter, bool bStopAtFirst = false)
{
    return FIrTextUtils::FindTopLevelDelimiterPositions(Str, Delimiter, bStopAtFirst);
}

TArray<FString> SmartSplit(const FString& Str, TCHAR Delimiter)
{
    return FIrTextUtils::SmartSplit(Str, Delimiter);
}

int32 FindMatchingChar(const FString& Str, int32 OpenPos, TCHAR OpenChar, TCHAR CloseChar)
{
    return FIrTextUtils::FindMatchingChar(Str, OpenPos, OpenChar, CloseChar);
}

int32 FindFirstTopLevelColon(const FString& Str)
{
    const TArray<int32> Positions = FindTopLevelDelimiterPositions(Str, TEXT(':'), true);
    return Positions.Num() > 0 ? Positions[0] : INDEX_NONE;
}

bool TryUnwrapNameToken(const FString& Text, FString& OutName)
{
    FString Error;
    return FIrTextUtils::TryUnwrapNameToken(Text, OutName, Error);
}

bool TryExtractPosition(FString& InOutLine, FVector2D& OutPosition, FString& OutError)
{
    return FIrTextUtils::TryExtractPosition(InOutLine, OutPosition, OutError);
}

bool IsQualifiedMaterialExpressionClassName(const FString& ClassName)
{
    int32 DotIndex = INDEX_NONE;
    return ClassName.StartsWith(TEXT("/Script/"))
        && ClassName.FindChar(TEXT('.'), DotIndex)
        && ClassName.Mid(DotIndex + 1).StartsWith(TEXT("MaterialExpression"));
}

bool TryExpandShortSubstrateClassName(const FString& ClassName, FString& OutClassPath)
{
    if (ClassName.StartsWith(TEXT("MaterialExpressionSubstrate"), ESearchCase::CaseSensitive))
    {
        OutClassPath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
        return true;
    }

    if (ClassName.StartsWith(TEXT("Substrate"), ESearchCase::CaseSensitive))
    {
        OutClassPath = FString::Printf(TEXT("/Script/Engine.MaterialExpression%s"), *ClassName);
        return true;
    }

    return false;
}

bool TryReadOpcodePrefix(const FString& Line, EMGIROpcode& OutOpcode, FString& OutRest)
{
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Line, FMGIRGrammar::Get());
    for (const FIrToken& Token : Tokens)
    {
        if (Token.Type == EIrTokenType::Comment)
        {
            break;
        }

        if (Token.Type == EIrTokenType::Keyword)
        {
            int32 OpcodeValue = 0;
            if (FMGIRGrammar::Get().TryGetOpcode(Token.Text, OpcodeValue))
            {
                OutOpcode = static_cast<EMGIROpcode>(OpcodeValue);
                OutRest = Line.Mid(Token.Position + Token.Text.Len()).TrimStart();
                return true;
            }
        }

        break;
    }

    return false;
}

bool IsEntryLine(const FString& Line)
{
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Line, FMGIRGrammar::Get());
    return Tokens.Num() > 0 &&
        Tokens[0].Type == EIrTokenType::Keyword &&
        Tokens[0].Text.ToLower() == TEXT("entry");
}

bool ReadCallLikeHead(const FString& Rest, FString& OutName, FString& OutArgs, FString& OutTypeText)
{
    FString Working = Rest.TrimStartAndEnd();
    OutTypeText.Reset();

    if (Working.StartsWith(TEXT("<")))
    {
        const int32 CloseAngle = FindMatchingChar(Working, 0, TEXT('<'), TEXT('>'));
        if (CloseAngle == INDEX_NONE)
        {
            return false;
        }
        OutTypeText = Working.Mid(1, CloseAngle - 1).TrimStartAndEnd();
        Working = Working.Mid(CloseAngle + 1).TrimStart();
    }

    const int32 OpenParen = Working.Find(TEXT("("));
    if (OpenParen == INDEX_NONE)
    {
        if (!TryUnwrapNameToken(Working, OutName))
        {
            return false;
        }
        OutArgs.Reset();
        return true;
    }

    const int32 CloseParen = FindMatchingChar(Working, OpenParen, TEXT('('), TEXT(')'));
    if (CloseParen == INDEX_NONE)
    {
        return false;
    }

    if (!TryUnwrapNameToken(Working.Left(OpenParen).TrimEnd(), OutName))
    {
        return false;
    }
    OutArgs = Working.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
    return true;
}
}

bool FMGIRParser::Parse(const FString& Code, TArray<FMGIREntryBlock>& OutBlocks, TArray<FMGIRParseError>& OutErrors)
{
    OutBlocks.Reset();
    OutErrors.Reset();

    TArray<FString> Lines;
    Code.ParseIntoArray(Lines, TEXT("\n"), false);

    FMGIREntryBlock* CurrentBlock = nullptr;
    bool bInsideBlock = false;

    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const int32 LineNum = Index + 1;
        FString Line = StripTrailingComment(Lines[Index]).TrimStartAndEnd();

        if (Line.IsEmpty())
        {
            continue;
        }

        if (IsEntryLine(Line))
        {
            FString EntryLine = Line;
            const bool bInlineEmptyBlock = EntryLine.EndsWith(TEXT("{}")) || EntryLine.EndsWith(TEXT("{ }"));
            if (bInlineEmptyBlock)
            {
                const int32 BracePos = EntryLine.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                EntryLine = EntryLine.Left(BracePos).TrimEnd();
            }
            else if (EntryLine.EndsWith(TEXT("{")))
            {
                EntryLine.LeftChopInline(1);
                EntryLine.TrimEndInline();
            }

            OutBlocks.AddDefaulted();
            CurrentBlock = &OutBlocks.Last();
            bInsideBlock = true;
            ParseEntryLine(EntryLine, LineNum, *CurrentBlock, OutErrors);

            if (bInlineEmptyBlock)
            {
                if (CurrentBlock)
                {
                    BuildIndices(*CurrentBlock, OutErrors);
                }
                CurrentBlock = nullptr;
                bInsideBlock = false;
            }
            continue;
        }

        if (Line == TEXT("{"))
        {
            continue;
        }

        if (Line == TEXT("}"))
        {
            if (bInsideBlock && CurrentBlock)
            {
                BuildIndices(*CurrentBlock, OutErrors);
            }
            CurrentBlock = nullptr;
            bInsideBlock = false;
            continue;
        }

        if (!bInsideBlock || !CurrentBlock)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Instruction outside of entry block: %s"), *Line));
            continue;
        }

        FMGIRInstruction Instruction;
        if (ParseInstruction(Line, LineNum, Instruction, OutErrors))
        {
            CurrentBlock->Instructions.Add(MoveTemp(Instruction));
        }
    }

    if (bInsideBlock && CurrentBlock)
    {
        AddError(OutErrors, Lines.Num(), TEXT("Missing closing '}' for entry block"));
    }

    return OutErrors.Num() == 0;
}

bool FMGIRParser::ParseEntryLine(const FString& Line, int32 LineNum, FMGIREntryBlock& OutBlock, TArray<FMGIRParseError>& OutErrors) const
{
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Line, FMGIRGrammar::Get());
    if (Tokens.Num() < 2)
    {
        AddError(OutErrors, LineNum, TEXT("Incomplete MGIR entry line; expected 'entry material' or 'entry function'"));
        return false;
    }

    const FString EntryToken = Tokens[0].Text.ToLower();
    const FString KindToken = Tokens[1].Text.ToLower();
    if (EntryToken != TEXT("entry"))
    {
        AddError(OutErrors, LineNum, TEXT("MGIR entry line must start with 'entry'"));
        return false;
    }

    if (KindToken == TEXT("material"))
    {
        OutBlock.Kind = EMGIREntryKind::Material;
    }
    else if (KindToken == TEXT("function"))
    {
        OutBlock.Kind = EMGIREntryKind::Function;
    }
    else
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unknown MGIR entry kind: %s"), *Tokens[1].Text));
        return false;
    }

    FString Rest = Line.Mid(Tokens[1].Position + Tokens[1].Text.Len()).TrimStartAndEnd();
    const int32 OpenParen = Rest.Find(TEXT("("));
    if (OpenParen != INDEX_NONE)
    {
        Rest = Rest.Left(OpenParen).TrimEnd();
    }

    FString NameError;
    if (!FIrTextUtils::TryUnwrapNameToken(Rest, OutBlock.Name, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid MGIR entry name token: %s"), *NameError),
            TEXT("MGIR_BAD_ENTRY"));
        return false;
    }
    return true;
}

bool FMGIRParser::ParseInstruction(const FString& Line, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const
{
    FString WorkingLine = Line;
    OutInst.SourceLine = LineNum;
    FString PositionError;
    OutInst.bHasPosition = TryExtractPosition(WorkingLine, OutInst.Position, PositionError);
    if (!PositionError.IsEmpty())
    {
        AddError(OutErrors, LineNum, PositionError);
        return false;
    }

    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(WorkingLine, FMGIRGrammar::Get());
    if (Tokens.Num() == 0)
    {
        return false;
    }

    if (Tokens[0].Type == EIrTokenType::PercentRef)
    {
        if (Tokens.Num() < 3 || Tokens[1].Type != EIrTokenType::Equals)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Expected assignment after %s"), *Tokens[0].Text));
            return false;
        }

        return ParseAssignedInstruction(Tokens[0].Text.Mid(1), WorkingLine.Mid(Tokens[1].Position + 1).TrimStart(), LineNum, OutInst, OutErrors);
    }

    EMGIROpcode Opcode = EMGIROpcode::Call;
    FString Rest;
    if (!TryReadOpcodePrefix(WorkingLine, Opcode, Rest))
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unknown MGIR instruction: %s"), *WorkingLine));
        return false;
    }

    if (Opcode == EMGIROpcode::Property)
    {
        return ParsePropertyInstruction(Rest, LineNum, OutInst, OutErrors);
    }

    if (Opcode != EMGIROpcode::Output)
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Instruction '%s' requires a result assignment."), *FMGIRGrammar::OpcodeToText(Opcode)));
        return false;
    }

    return ParseOutputInstruction(Rest, LineNum, OutInst, OutErrors);
}

bool FMGIRParser::ParseAssignedInstruction(const FString& ResultName, const FString& Rest, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const
{
    if (ResultName.IsEmpty())
    {
        AddError(OutErrors, LineNum, TEXT("MGIR result name is empty"));
        return false;
    }

    EMGIROpcode Opcode = EMGIROpcode::Call;
    FString OpcodeRest;
    if (!TryReadOpcodePrefix(Rest, Opcode, OpcodeRest))
    {
        FString SugarHeadName;
        FString SugarArgsText;
        FString SugarTypeText;
        FString SugarClassPath;
        if (!ReadCallLikeHead(Rest, SugarHeadName, SugarArgsText, SugarTypeText)
            || !FMGIRSubstrateSugar::TryGetClassPathForAlias(SugarHeadName, SugarClassPath))
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Expected MGIR opcode after assignment to %%%s"), *ResultName));
            return false;
        }

        OutInst.Opcode = EMGIROpcode::Call;
        OutInst.ResultName = ResultName;
        OutInst.SymbolName = SugarClassPath;
        OutInst.TypeText = SugarTypeText;
        OutInst.Args = ParseArgs(SugarArgsText);
        if (!SugarTypeText.IsEmpty())
        {
            FMGIRTypeGrammar::TryParseType(SugarTypeText, OutInst.ValueType);
        }
        return true;
    }

    if (Opcode == EMGIROpcode::Output)
    {
        AddError(OutErrors, LineNum, TEXT("output instructions cannot be assigned to a result"));
        return false;
    }

    if (Opcode == EMGIROpcode::Property)
    {
        AddError(OutErrors, LineNum, TEXT("property instructions cannot be assigned to a result"));
        return false;
    }

    FString HeadName;
    FString ArgsText;
    FString TypeText;
    if (!ReadCallLikeHead(OpcodeRest, HeadName, ArgsText, TypeText))
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Malformed %s instruction"), *FMGIRGrammar::OpcodeToText(Opcode)));
        return false;
    }

    OutInst.Opcode = Opcode;
    OutInst.ResultName = ResultName;
    OutInst.SymbolName = HeadName;
    OutInst.TypeText = TypeText;
    OutInst.Args = ParseArgs(ArgsText);

    if (Opcode == EMGIROpcode::Call && !IsQualifiedMaterialExpressionClassName(HeadName))
    {
        FString ExpandedSubstrateClassPath;
        if (TryExpandShortSubstrateClassName(HeadName, ExpandedSubstrateClassPath))
        {
            OutInst.SymbolName = ExpandedSubstrateClassPath;
        }
        else
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("MGIR call class names must be qualified material expression class paths, not '%s'"), *HeadName),
                TEXT("MGIR_BAD_CLASS_NAME"));
            return false;
        }
    }

    if (!TypeText.IsEmpty())
    {
        FMGIRTypeGrammar::TryParseType(TypeText, OutInst.ValueType);
    }
    else if (Opcode == EMGIROpcode::Constant && FMGIRTypeGrammar::TryParseType(HeadName, OutInst.ValueType))
    {
        OutInst.TypeText = FMGIRTypeGrammar::TypeToText(OutInst.ValueType);
        OutInst.SymbolName.Reset();
    }

    if (Opcode == EMGIROpcode::Constant && OutInst.ValueType == EMGIRValueType::Unknown)
    {
        AddError(OutErrors, LineNum, TEXT("constant requires a material value type such as Float1 or Float3"));
        return false;
    }

    return true;
}

bool FMGIRParser::ParseOutputInstruction(const FString& Rest, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const
{
    const int32 ColonIndex = FindFirstTopLevelColon(Rest);
    if (ColonIndex == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("output requires 'Name: Value'"));
        return false;
    }

    OutInst.Opcode = EMGIROpcode::Output;
    OutInst.OutputName = Rest.Left(ColonIndex).TrimStartAndEnd();
    OutInst.Value = Rest.Mid(ColonIndex + 1).TrimStartAndEnd();

    if (OutInst.OutputName.IsEmpty() || OutInst.Value.IsEmpty())
    {
        AddError(OutErrors, LineNum, TEXT("output requires non-empty name and value"));
        return false;
    }

    return true;
}

// `property <Name>: <Value>` - material-level UMaterial state, not a graph node. Shares the
// `Name: Value` shape with `output` but is kept a distinct opcode so the compiler can refuse
// it in a function block, where none of these properties exist.
bool FMGIRParser::ParsePropertyInstruction(const FString& Rest, int32 LineNum, FMGIRInstruction& OutInst, TArray<FMGIRParseError>& OutErrors) const
{
    const int32 ColonIndex = FindFirstTopLevelColon(Rest);
    if (ColonIndex == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("property requires 'Name: Value'"), TEXT("MGIR_BAD_PROPERTY"));
        return false;
    }

    OutInst.Opcode = EMGIROpcode::Property;
    OutInst.SymbolName = Rest.Left(ColonIndex).TrimStartAndEnd();
    OutInst.Value = Rest.Mid(ColonIndex + 1).TrimStartAndEnd();

    if (OutInst.SymbolName.IsEmpty() || OutInst.Value.IsEmpty())
    {
        AddError(OutErrors, LineNum, TEXT("property requires non-empty name and value"), TEXT("MGIR_BAD_PROPERTY"));
        return false;
    }

    return true;
}

bool FMGIRParser::BuildIndices(FMGIREntryBlock& Block, TArray<FMGIRParseError>& OutErrors) const
{
    const int32 InitialErrors = OutErrors.Num();
    Block.ValueIndex.Reset();

    for (int32 Index = 0; Index < Block.Instructions.Num(); ++Index)
    {
        const FMGIRInstruction& Instruction = Block.Instructions[Index];
        if (Instruction.ResultName.IsEmpty())
        {
            continue;
        }

        if (Block.ValueIndex.Contains(Instruction.ResultName))
        {
            AddError(OutErrors, Instruction.SourceLine,
                FString::Printf(TEXT("Duplicate MGIR result name: %%%s"), *Instruction.ResultName));
            continue;
        }

        Block.ValueIndex.Add(Instruction.ResultName, Index);
    }

    return OutErrors.Num() == InitialErrors;
}

TArray<FMGIRArg> FMGIRParser::ParseArgs(const FString& ArgsText) const
{
    TArray<FMGIRArg> Args;
    const FString TrimmedArgs = ArgsText.TrimStartAndEnd();
    if (TrimmedArgs.IsEmpty())
    {
        return Args;
    }

    for (const FString& Part : SmartSplit(TrimmedArgs, TEXT(',')))
    {
        const FString TrimmedPart = Part.TrimStartAndEnd();
        if (TrimmedPart.IsEmpty())
        {
            continue;
        }

        FMGIRArg Arg;
        const int32 ColonIndex = FindFirstTopLevelColon(TrimmedPart);
        if (ColonIndex != INDEX_NONE)
        {
            Arg.Name = TrimmedPart.Left(ColonIndex).TrimStartAndEnd();
            Arg.Value = TrimmedPart.Mid(ColonIndex + 1).TrimStartAndEnd();
        }
        else
        {
            Arg.Value = TrimmedPart;
        }

        Args.Add(MoveTemp(Arg));
    }

    return Args;
}
