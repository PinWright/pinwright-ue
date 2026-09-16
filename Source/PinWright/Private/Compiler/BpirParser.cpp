// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirParser.cpp - Line-based parser for BPIR (Blueprint IR) text

#include "Compiler/BpirParser.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirGrammar.h"
#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/CompilerTypes.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/BpirSharedConstants.h"
#include "IrCore/IIrGrammar.h"
#include "IrCore/IrTextUtils.h"
#include "IrCore/IrTokenizer.h"
#include "Utils/PropertyUtils.h"

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static void AddError(TArray<FCompileError>& Errors, int32 Line, const FString& Msg)
{
    Errors.Add(FCompileError(Line, Msg));
}

static FString UnwrapNameTokenOrLegacy(const FString& Text, bool* bOutWasNameToken = nullptr)
{
    FString Unwrapped;
    FString Error;
    if (FIrTextUtils::TryUnwrapNameToken(Text, Unwrapped, Error))
    {
        if (bOutWasNameToken)
        {
            *bOutWasNameToken = true;
        }
        return Unwrapped;
    }

    if (bOutWasNameToken)
    {
        *bOutWasNameToken = false;
    }
    return Text.TrimStartAndEnd();
}

// Splits "label" or "label.pin" or "`label name`.`pin name`" or "`label name`.pin"
// into (Label, InputPin). Returns true on success; false on syntax error
// (e.g. unterminated backtick, multiple unquoted dots).
static bool SplitLabelAndInputPin(const FString& In, FString& OutLabel, FString& OutInputPin)
{
    OutLabel.Reset();
    OutInputPin.Reset();

    int32 i = 0;
    const int32 N = In.Len();
    while (i < N && FChar::IsWhitespace(In[i])) ++i;

    auto ConsumeSegment = [&](FString& Out) -> bool
    {
        if (i < N && In[i] == TEXT('`'))
        {
            int32 Start = ++i;
            while (i < N && In[i] != TEXT('`')) ++i;
            if (i >= N) return false;        // unterminated backtick
            Out = In.Mid(Start, i - Start);
            ++i;                              // consume closing backtick
        }
        else
        {
            int32 Start = i;
            while (i < N && In[i] != TEXT('.')) ++i;
            Out = In.Mid(Start, i - Start).TrimStartAndEnd();
        }
        return true;
    };

    if (!ConsumeSegment(OutLabel)) return false;
    if (i < N && In[i] == TEXT('.'))
    {
        ++i;
        if (!ConsumeSegment(OutInputPin)) return false;
        // Reject anything trailing past the second segment.
        while (i < N && FChar::IsWhitespace(In[i])) ++i;
        if (i < N) return false;
    }
    return true;
}

// Strip trailing inline comment from a value, respecting quoted strings.
// "42 # comment" -> "42"
// "\"hello # world\" # comment" -> "\"hello # world\""
static FString StripTrailingComment(const FString& Value)
{
    bool bInQuote = false;
    for (int32 i = 0; i < Value.Len(); ++i)
    {
        TCHAR Ch = Value[i];
        if (Ch == TEXT('"'))
        {
            // Count preceding backslashes to determine if escaped
            int32 BackslashCount = 0;
            for (int32 j = i - 1; j >= 0 && Value[j] == TEXT('\\'); --j)
            {
                ++BackslashCount;
            }
            if (BackslashCount % 2 == 0)
            {
                bInQuote = !bInQuote;
            }
            continue;
        }
        if (!bInQuote && Ch == TEXT('#'))
        {
            return Value.Left(i).TrimEnd();
        }
    }
    return Value;
}

// Check if a quote character at position i is unescaped (not preceded by an odd number of backslashes)
static bool IsUnescapedQuote(const FString& Str, int32 i)
{
    if (i >= Str.Len() || Str[i] != TEXT('"'))
    {
        return false;
    }
    int32 BackslashCount = 0;
    for (int32 j = i - 1; j >= 0 && Str[j] == TEXT('\\'); --j)
    {
        ++BackslashCount;
    }
    return BackslashCount % 2 == 0;
}

// Split a string by a delimiter, respecting nested parens, angle brackets, curly braces, and quoted strings
static TArray<FString> SmartSplit(const FString& Str, TCHAR Delim)
{
    return FIrTextUtils::SmartSplit(Str, Delim);
}

static int32 FindLastWhitespaceOutsideNameToken(const FString& Text)
{
    bool bInName = false;
    int32 LastSpace = INDEX_NONE;
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const TCHAR Ch = Text[Index];
        if (Ch == TEXT('`') && FIrTextUtils::IsUnescapedBacktick(Text, Index))
        {
            bInName = !bInName;
            continue;
        }
        if (!bInName && FChar::IsWhitespace(Ch))
        {
            LastSpace = Index;
        }
    }
    return LastSpace;
}

static bool IsNodeEnabledStateToken(const FString& Text)
{
    return Text.Equals(BpirSharedConstants::Keywords::Enabled, ESearchCase::IgnoreCase)
        || Text.Equals(BpirSharedConstants::Keywords::Disabled, ESearchCase::IgnoreCase)
        || Text.Equals(BpirSharedConstants::Keywords::DevelopmentOnly, ESearchCase::IgnoreCase);
}

static bool ExtractNodeEnabledStateSuffix(
    FString& InOutLine,
    EBpirNodeEnabledState& OutState,
    bool& bOutHasState,
    FString& OutError)
{
    OutState = EBpirNodeEnabledState::Enabled;
    bOutHasState = false;
    OutError.Reset();

    int32 CommentStart = INDEX_NONE;
    bool bInQuote = false;
    bool bInNameToken = false;
    for (int32 Index = 0; Index < InOutLine.Len(); ++Index)
    {
        if (InOutLine[Index] == TEXT('"') && IsUnescapedQuote(InOutLine, Index))
        {
            bInQuote = !bInQuote;
        }
        else if (!bInQuote && InOutLine[Index] == TEXT('`')
            && FIrTextUtils::IsUnescapedBacktick(InOutLine, Index))
        {
            bInNameToken = !bInNameToken;
        }
        else if (!bInQuote && !bInNameToken && InOutLine[Index] == TEXT('#'))
        {
            CommentStart = Index;
            break;
        }
    }

    const FString CodePart = (CommentStart == INDEX_NONE
        ? InOutLine
        : InOutLine.Left(CommentStart)).TrimEnd();
    FString Remaining = CodePart;

    auto ConsumeState = [&Remaining, &OutState, &bOutHasState](const TCHAR* Token, EBpirNodeEnabledState State) -> bool
    {
        if (!Remaining.EndsWith(Token, ESearchCase::IgnoreCase))
        {
            return false;
        }

        const int32 TokenStart = Remaining.Len() - FCString::Strlen(Token);
        if (TokenStart > 0 && !FChar::IsWhitespace(Remaining[TokenStart - 1]))
        {
            return false;
        }

        Remaining = Remaining.Left(TokenStart).TrimEnd();
        OutState = State;
        bOutHasState = true;
        return true;
    };

    if (ConsumeState(BpirSharedConstants::Keywords::DevelopmentOnly, EBpirNodeEnabledState::DevelopmentOnly)
        || ConsumeState(BpirSharedConstants::Keywords::Disabled, EBpirNodeEnabledState::Disabled)
        || ConsumeState(BpirSharedConstants::Keywords::Enabled, EBpirNodeEnabledState::Enabled))
    {
        // A second marker is almost certainly a typo. Report it instead of
        // leaving the first one to be interpreted as an instruction argument.
        const int32 LastSpace = FindLastWhitespaceOutsideNameToken(Remaining);
        const FString PossibleSecond = LastSpace == INDEX_NONE
            ? Remaining
            : Remaining.Mid(LastSpace + 1).TrimStartAndEnd();
        if (IsNodeEnabledStateToken(PossibleSecond))
        {
            OutError = TEXT("Only one node enabled-state marker is allowed per BPIR line");
            return false;
        }
    }

    if (CommentStart == INDEX_NONE)
    {
        InOutLine = Remaining;
    }
    else
    {
        InOutLine = Remaining + InOutLine.Mid(CommentStart);
    }
    return true;
}

bool FBpirParser::TryExtractAuthoredPosition(FString& InOutLine, FVector2D& OutPosition, FString& OutError)
{
    OutError.Reset();
    TArray<int32> MarkerPositions;
    bool bInQuote = false;

    for (int32 Index = 0; Index < InOutLine.Len() - 1; ++Index)
    {
        if (InOutLine[Index] == TEXT('"') && IsUnescapedQuote(InOutLine, Index))
        {
            bInQuote = !bInQuote;
            continue;
        }
        if (!bInQuote && InOutLine[Index] == TEXT('#'))
        {
            break;
        }
        if (!bInQuote && InOutLine[Index] == TEXT('@') && InOutLine[Index + 1] == TEXT('('))
        {
            MarkerPositions.Add(Index);
        }
    }

    if (MarkerPositions.Num() == 0)
    {
        return false;
    }
    if (MarkerPositions.Num() > 1)
    {
        OutError = TEXT("Only one '@(x, y)' position marker is allowed per BPIR instruction");
        return false;
    }

    const int32 MarkerPos = MarkerPositions[0];
    const int32 OpenParen = MarkerPos + 1;
    const int32 CloseParen = FBpirParser::FindMatchingParen(InOutLine, OpenParen);
    if (CloseParen == INDEX_NONE)
    {
        OutError = TEXT("Unmatched '@(' position marker");
        return false;
    }

    const FString Suffix = InOutLine.Mid(CloseParen + 1).TrimStartAndEnd();
    const int32 SuffixCommentStart = Suffix.Find(TEXT("#"));
    const FString SuffixCode = (SuffixCommentStart == INDEX_NONE
        ? Suffix
        : Suffix.Left(SuffixCommentStart)).TrimStartAndEnd();
    if (!SuffixCode.IsEmpty() && !IsNodeEnabledStateToken(SuffixCode))
    {
        OutError = TEXT("Position marker must be the trailing suffix: '@(x, y)'");
        return false;
    }

    const FString Contents = InOutLine.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
    const TArray<FString> Parts = SmartSplit(Contents, TEXT(','));
    if (Parts.Num() != 2)
    {
        OutError = TEXT("Position marker requires '@(x, y)'");
        return false;
    }

    const FString XText = Parts[0].TrimStartAndEnd();
    const FString YText = Parts[1].TrimStartAndEnd();
    if (!LexTryParseString(OutPosition.X, *XText) ||
        !LexTryParseString(OutPosition.Y, *YText))
    {
        OutError = TEXT("Position marker coordinates must be numeric");
        return false;
    }

    InOutLine = (InOutLine.Left(MarkerPos) + InOutLine.Mid(CloseParen + 1)).TrimStartAndEnd();
    return true;
}

static void ExtractExecPinNames(const FString& BracketContent, TArray<FString>& OutNames)
{
    TArray<FString> Parts = SmartSplit(BracketContent, TEXT(','));
    for (const FString& Part : Parts)
    {
        FString Trimmed = Part.TrimStartAndEnd();
        int32 ArrowIdx = Trimmed.Find(TEXT("->"));
        if (ArrowIdx != INDEX_NONE)
        {
            OutNames.Add(Trimmed.Left(ArrowIdx).TrimEnd());
        }
    }
}

// ----------------------------------------------------------------------------
// FindMatchingParen / FindMatchingBrace / FindMatchingBracket
// ----------------------------------------------------------------------------

int32 FBpirParser::FindMatchingParen(const FString& Str, int32 OpenPos)
{
    if (OpenPos >= Str.Len() || Str[OpenPos] != TEXT('('))
    {
        return INDEX_NONE;
    }

    int32 Depth = 1;
    bool bInQuote = false;

    for (int32 i = OpenPos + 1; i < Str.Len(); ++i)
    {
        TCHAR Ch = Str[i];

        if (Ch == TEXT('"') && IsUnescapedQuote(Str, i))
        {
            bInQuote = !bInQuote;
            continue;
        }

        if (bInQuote)
        {
            continue;
        }

        if (Ch == TEXT('('))
        {
            ++Depth;
        }
        else if (Ch == TEXT(')'))
        {
            --Depth;
            if (Depth == 0)
            {
                return i;
            }
        }
    }

    return INDEX_NONE;
}

int32 FBpirParser::FindMatchingBrace(const FString& Str, int32 OpenPos)
{
    if (OpenPos >= Str.Len() || Str[OpenPos] != TEXT('{'))
    {
        return INDEX_NONE;
    }

    int32 Depth = 1;
    bool bInQuote = false;

    for (int32 i = OpenPos + 1; i < Str.Len(); ++i)
    {
        TCHAR Ch = Str[i];

        if (Ch == TEXT('"') && IsUnescapedQuote(Str, i))
        {
            bInQuote = !bInQuote;
            continue;
        }

        if (bInQuote)
        {
            continue;
        }

        if (Ch == TEXT('{'))
        {
            ++Depth;
        }
        else if (Ch == TEXT('}'))
        {
            --Depth;
            if (Depth == 0)
            {
                return i;
            }
        }
    }

    return INDEX_NONE;
}

int32 FBpirParser::FindMatchingBracket(const FString& Str, int32 OpenPos)
{
    if (OpenPos >= Str.Len() || Str[OpenPos] != TEXT('['))
    {
        return INDEX_NONE;
    }

    int32 Depth = 1;
    bool bInQuote = false;

    for (int32 i = OpenPos + 1; i < Str.Len(); ++i)
    {
        TCHAR Ch = Str[i];

        if (Ch == TEXT('"') && IsUnescapedQuote(Str, i))
        {
            bInQuote = !bInQuote;
            continue;
        }

        if (bInQuote)
        {
            continue;
        }

        if (Ch == TEXT('['))
        {
            ++Depth;
        }
        else if (Ch == TEXT(']'))
        {
            --Depth;
            if (Depth == 0)
            {
                return i;
            }
        }
    }

    return INDEX_NONE;
}

int32 FBpirParser::FindMatchingAngle(const FString& Str, int32 OpenPos)
{
    if (OpenPos >= Str.Len() || Str[OpenPos] != TEXT('<'))
    {
        return INDEX_NONE;
    }

    int32 Depth = 1;
    bool bInQuote = false;

    for (int32 i = OpenPos + 1; i < Str.Len(); ++i)
    {
        TCHAR Ch = Str[i];

        if (Ch == TEXT('"') && IsUnescapedQuote(Str, i))
        {
            bInQuote = !bInQuote;
            continue;
        }

        if (bInQuote)
        {
            continue;
        }

        if (Ch == TEXT('<'))
        {
            ++Depth;
        }
        else if (Ch == TEXT('>'))
        {
            --Depth;
            if (Depth == 0)
            {
                return i;
            }
        }
    }

    return INDEX_NONE;
}

// ----------------------------------------------------------------------------
// NormalizePinName
// ----------------------------------------------------------------------------

FString FBpirParser::NormalizePinName(const FString& Name)
{
    FString Result;
    Result.Reserve(Name.Len());

    bool bCapNext = false;
    for (int32 i = 0; i < Name.Len(); ++i)
    {
        TCHAR Ch = Name[i];
        if (FChar::IsWhitespace(Ch))
        {
            bCapNext = true;
            continue;
        }

        if (bCapNext)
        {
            Result.AppendChar(FChar::ToUpper(Ch));
            bCapNext = false;
        }
        else
        {
            Result.AppendChar(Ch);
        }
    }

    return Result;
}

// ----------------------------------------------------------------------------
// Parse (main entry point)
// ----------------------------------------------------------------------------

bool FBpirParser::Parse(const FString& Code, TArray<FBpirEntryBlock>& OutBlocks, TArray<FCompileError>& OutErrors, bool bSkipReferenceValidation)
{
    TArray<FString> Lines;
    Code.ParseIntoArray(Lines, TEXT("\n"), false);

    FBpirEntryBlock* CurrentBlock = nullptr;
    bool bInsideBlock = false;

    // Decorator state — accumulated while consuming consecutive `@meta(...)` /
    // `@flags(...)` lines above an `entry …` signature. Reset (with error) if a
    // blank/comment/other line appears between the decorator(s) and the entry.
    FBpirEntryMetadata PendingMetadata;
    bool bPendingMetadataActive = false;
    int32 PendingMetadataFirstLineNum = 0;

    auto ResetPending = [&]()
    {
        PendingMetadata = FBpirEntryMetadata{};
        bPendingMetadataActive = false;
        PendingMetadataFirstLineNum = 0;
    };

    for (int32 i = 0; i < Lines.Num(); ++i)
    {
        int32 LineNum = i + 1;
        FString Line = Lines[i].TrimStartAndEnd();

        // Skip blank lines and comment-only lines
        if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
        {
            // If inside a block, add comments as instructions
            if (bInsideBlock && CurrentBlock && Line.StartsWith(TEXT("#")))
            {
                FBpirInstruction Inst;
                Inst.Opcode = EBpirOpcode::Comment;
                Inst.FunctionName = Line.Mid(1).TrimStart();
                Inst.SourceLine = LineNum;
                CurrentBlock->Instructions.Add(MoveTemp(Inst));
            }
            // Pending decorator metadata requires an `entry …` line on the very
            // next non-decorator line — a blank line or comment between them
            // breaks the binding.
            if (!bInsideBlock && bPendingMetadataActive)
            {
                AddError(OutErrors, PendingMetadataFirstLineNum,
                    TEXT("@meta(...) / @flags(...) decorators must be immediately above an 'entry …' line (no blank lines or comments between)"));
                ResetPending();
            }
            continue;
        }

        // Decorator line above an entry signature (only valid at top level)
        if (!bInsideBlock && (Line.StartsWith(TEXT("@meta(")) || Line.StartsWith(TEXT("@flags("))))
        {
            if (!bPendingMetadataActive)
            {
                PendingMetadataFirstLineNum = LineNum;
                bPendingMetadataActive = true;
            }
            if (Line.StartsWith(TEXT("@meta(")))
            {
                ParseMetaDecorator(Line, LineNum, PendingMetadata, OutErrors);
            }
            else
            {
                ParseFlagsDecorator(Line, LineNum, PendingMetadata, OutErrors);
            }
            continue;
        }

        // Check for entry line
        if (Line.StartsWith(TEXT("entry ")))
        {
            // Check for empty inline block: "entry ... { }"
            bool bInlineEmptyBlock = false;
            FString EntryLine = Line;
            if (EntryLine.EndsWith(TEXT("{ }")) || EntryLine.EndsWith(TEXT("{}")) )
            {
                // Strip the trailing "{ }" or "{}" to get the clean entry line
                int32 BracePos = EntryLine.Find(TEXT("{"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
                if (BracePos != INDEX_NONE)
                {
                    EntryLine = EntryLine.Left(BracePos).TrimEnd();
                }
                bInlineEmptyBlock = true;
            }
            // Remove trailing '{' if present (multi-line block)
            else if (EntryLine.EndsWith(TEXT("{")))
            {
                EntryLine = EntryLine.Left(EntryLine.Len() - 1).TrimEnd();
            }

            OutBlocks.AddDefaulted();
            CurrentBlock = &OutBlocks.Last();
            bInsideBlock = true;

            if (bPendingMetadataActive)
            {
                CurrentBlock->Metadata = PendingMetadata;
                ResetPending();
            }

            if (!ParseEntryLine(EntryLine, LineNum, *CurrentBlock, OutErrors))
            {
                // Error already reported; keep parsing for more errors
            }

            // Reject @flags on macros / both on engine events. Kind is set by
            // ParseEntryLine above.
            ValidateMetadataKindRestrictions(*CurrentBlock, LineNum, OutErrors);

            // For inline empty blocks, immediately close the block
            if (bInlineEmptyBlock)
            {
                if (CurrentBlock)
                {
                    BuildIndicesAndValidate(*CurrentBlock, OutErrors, bSkipReferenceValidation);
                }
                bInsideBlock = false;
                CurrentBlock = nullptr;
            }
            continue;
        }

        // Any other line at top-level invalidates pending decorators — they
        // must bind to the immediately-following `entry …` line.
        if (!bInsideBlock && bPendingMetadataActive)
        {
            AddError(OutErrors, PendingMetadataFirstLineNum,
                TEXT("@meta(...) / @flags(...) decorators must be immediately above an 'entry …' line"));
            ResetPending();
        }

        // Check for closing brace
        if (Line == TEXT("}"))
        {
            if (bInsideBlock && CurrentBlock)
            {
                BuildIndicesAndValidate(*CurrentBlock, OutErrors, bSkipReferenceValidation);
            }
            bInsideBlock = false;
            CurrentBlock = nullptr;
            continue;
        }

        // Stray opening brace on its own line (multi-line entry form where the
        // author put `{` below the signature instead of on the same line).
        // ParseEntryLine already strips an inline trailing `{`, so the only
        // way to land here is a bare `{` inside the block — skip it.
        if (bInsideBlock && Line == TEXT("{"))
        {
            continue;
        }

        // Must be inside a block to parse instructions
        if (!bInsideBlock || !CurrentBlock)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Instruction outside of entry block: %s"), *Line));
            continue;
        }

        FBpirInstruction Inst;
        if (ParseInstruction(Line, LineNum, Inst, OutErrors))
        {
            CurrentBlock->Instructions.Add(MoveTemp(Inst));
        }
    }

    // If still inside a block at end, the closing '}' is missing
    if (bInsideBlock && CurrentBlock)
    {
        AddError(OutErrors, Lines.Num(), TEXT("Missing closing '}' for entry block"));
    }

    // Pending decorators with no following entry line
    if (bPendingMetadataActive)
    {
        AddError(OutErrors, PendingMetadataFirstLineNum,
            TEXT("@meta(...) / @flags(...) decorators must be immediately above an 'entry …' line (none found)"));
    }

    return OutErrors.Num() == 0;
}

// ----------------------------------------------------------------------------
// ParseBody (headless mode)
// ----------------------------------------------------------------------------

bool FBpirParser::ParseBody(const FString& Code, FBpirEntryBlock& OutBlock, TArray<FCompileError>& OutErrors)
{
    TArray<FString> Lines;
    Code.ParseIntoArray(Lines, TEXT("\n"), false);

    for (int32 i = 0; i < Lines.Num(); ++i)
    {
        int32 LineNum = i + 1;
        FString Line = Lines[i].TrimStartAndEnd();

        // Skip blank lines
        if (Line.IsEmpty())
        {
            continue;
        }

        // Comments
        if (Line.StartsWith(TEXT("#")))
        {
            FBpirInstruction Inst;
            Inst.Opcode = EBpirOpcode::Comment;
            Inst.FunctionName = Line.Mid(1).TrimStart();
            Inst.SourceLine = LineNum;
            OutBlock.Instructions.Add(MoveTemp(Inst));
            continue;
        }

        // Skip stray braces in body mode
        if (Line == TEXT("{") || Line == TEXT("}"))
        {
            continue;
        }

        FBpirInstruction Inst;
        if (ParseInstruction(Line, LineNum, Inst, OutErrors))
        {
            OutBlock.Instructions.Add(MoveTemp(Inst));
        }
    }

    BuildIndicesAndValidate(OutBlock, OutErrors);
    return OutErrors.Num() == 0;
}

// ----------------------------------------------------------------------------
// ParseEntryLine
// ----------------------------------------------------------------------------

bool FBpirParser::ParseEntryLine(const FString& Line, int32 LineNum, FBpirEntryBlock& OutBlock, TArray<FCompileError>& OutErrors)
{
    // Line format: "entry <kind> <name>(<params>) [-> <returntype>]"
    // Already has "entry " prefix stripped by caller? No, caller passes full line.

    FString WorkingLine = Line;
    FString PositionError;
    OutBlock.AuthoredEntryPosition = FVector2D::ZeroVector;
    OutBlock.bHasAuthoredEntryPosition = TryExtractAuthoredPosition(WorkingLine, OutBlock.AuthoredEntryPosition, PositionError);
    if (!PositionError.IsEmpty())
    {
        AddError(OutErrors, LineNum, PositionError);
        return false;
    }

    FString EnabledStateError;
    if (!ExtractNodeEnabledStateSuffix(
            WorkingLine,
            OutBlock.EnabledState,
            OutBlock.bHasEnabledState,
            EnabledStateError))
    {
        AddError(OutErrors, LineNum, EnabledStateError);
        return false;
    }

    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(WorkingLine, GetBpirGrammar());

    // Expect at least 2 tokens: "entry" keyword + kind keyword
    if (Tokens.Num() < 2)
    {
        AddError(OutErrors, LineNum, TEXT("Incomplete entry line"));
        return false;
    }

    FString EntryToken = Tokens[0].Text;
    FString KindToken = Tokens[1].Text;

    // Determine entry kind
    if (KindToken == TEXT("event"))
    {
        OutBlock.Kind = EBpirEntryKind::Event;
    }
    else if (KindToken == TEXT("custom_event"))
    {
        OutBlock.Kind = EBpirEntryKind::CustomEvent;
    }
    else if (KindToken == TEXT("function"))
    {
        OutBlock.Kind = EBpirEntryKind::Function;
    }
    else if (KindToken == TEXT("override"))
    {
        OutBlock.Kind = EBpirEntryKind::Override;
    }
    else if (KindToken == TEXT("construction"))
    {
        OutBlock.Kind = EBpirEntryKind::Construction;
    }
    else if (KindToken == TEXT("component_event"))
    {
        OutBlock.Kind = EBpirEntryKind::ComponentEvent;
    }
    else if (KindToken == TEXT("widget_event"))
    {
        OutBlock.Kind = EBpirEntryKind::WidgetEvent;
    }
    else if (KindToken == TEXT("key_pressed"))
    {
        OutBlock.Kind = EBpirEntryKind::KeyPressed;
    }
    else if (KindToken == TEXT("key_released"))
    {
        OutBlock.Kind = EBpirEntryKind::KeyReleased;
    }
    else if (KindToken == TEXT("input_action"))
    {
        OutBlock.Kind = EBpirEntryKind::InputAction;
    }
    else if (KindToken == TEXT("macro"))
    {
        OutBlock.Kind = EBpirEntryKind::Macro;
    }
    else
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unknown entry kind: '%s'"), *KindToken));
        return false;
    }

    // Rest of line after kind token
    int32 RestPos = Tokens[1].Position + Tokens[1].Text.Len();
    FString Rest = WorkingLine.Mid(RestPos).TrimStart();

    // Find the opening paren
    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '(' in entry signature"));
        return false;
    }

    // Name is everything before the paren (may include Component.Event for component_event)
    FString NamePart = Rest.Left(ParenOpen).TrimEnd();

    // Unwrap backtick-quoted names (e.g. `Update Opponents`, `Set Error`) — applies
    // to any entry kind whose name may contain spaces.
    NamePart = UnwrapNameTokenOrLegacy(NamePart);

    // Handle component_event / widget_event: "ComponentName.EventName"
    if (OutBlock.Kind == EBpirEntryKind::ComponentEvent || OutBlock.Kind == EBpirEntryKind::WidgetEvent)
    {
        int32 DotIdx = INDEX_NONE;
        NamePart.FindChar(TEXT('.'), DotIdx);
        if (DotIdx != INDEX_NONE)
        {
            OutBlock.ComponentName = NamePart.Left(DotIdx);
            OutBlock.Name = NamePart.Mid(DotIdx + 1);
        }
        else
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Expected 'ComponentName.EventName' for %s, got '%s'"),
                OutBlock.Kind == EBpirEntryKind::ComponentEvent ? TEXT("component_event") : TEXT("widget_event"), *NamePart));
            return false;
        }
    }
    else
    {
        OutBlock.Name = NamePart;

        // Reject names containing '@', ':', or '.' — these indicate the user wrote
        // `entry event @Widget:Event(...)` instead of the correct
        // `entry widget_event Widget.Event()` form.
        if (OutBlock.Name.Contains(TEXT("@")) || OutBlock.Name.Contains(TEXT(":"))
            || (OutBlock.Kind != EBpirEntryKind::InputAction && OutBlock.Name.Contains(TEXT("."))))
        {
            // Try to parse out X and Y from @X:Y to produce a targeted suggestion.
            FString SuggestionMsg;
            if (OutBlock.Name.StartsWith(TEXT("@")) && OutBlock.Name.Contains(TEXT(":")))
            {
                FString WithoutAt = OutBlock.Name.Mid(1); // strip leading '@'
                FString WidgetPart;
                FString EventPart;
                if (WithoutAt.Split(TEXT(":"), &WidgetPart, &EventPart))
                {
                    SuggestionMsg = FString::Printf(
                        TEXT("Invalid event name '%s'. Did you mean 'entry widget_event %s.%s()'? The '@Name:Event' form is not supported — use 'widget_event' for widget delegate bindings."),
                        *OutBlock.Name, *WidgetPart, *EventPart);
                }
            }
            if (SuggestionMsg.IsEmpty())
            {
                SuggestionMsg = FString::Printf(
                    TEXT("Invalid event name '%s': names for '%s' entries may not contain '@', ':', or '.'"),
                    *OutBlock.Name, *KindToken);
            }
            AddError(OutErrors, LineNum, SuggestionMsg);
            return false;
        }
    }

    // Validate non-empty entry name
    if (OutBlock.Name.IsEmpty())
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Empty entry name for '%s'"), *KindToken));
        return false;
    }

    if (OutBlock.Kind == EBpirEntryKind::InputAction)
    {
        const int32 DotIndex = OutBlock.Name.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (!OutBlock.Name.StartsWith(TEXT("/")) || DotIndex <= 1 || DotIndex == OutBlock.Name.Len() - 1)
        {
            AddError(OutErrors, LineNum,
                TEXT("input_action requires a full object path such as '/Game/Input/IA_Move.IA_Move'"));
            return false;
        }
    }

    // Parse parameters between parens
    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '(' in entry signature"));
        return false;
    }

    FString ParamsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1).TrimStartAndEnd();
    if (OutBlock.Kind == EBpirEntryKind::InputAction && !ParamsStr.IsEmpty())
    {
        AddError(OutErrors, LineNum, TEXT("input_action entries do not accept parameters"));
        return false;
    }
    if (!ParamsStr.IsEmpty())
    {
        TArray<FString> ParamParts = SmartSplit(ParamsStr, TEXT(','));
        for (const FString& Part : ParamParts)
        {
            FString Trimmed = Part.TrimStartAndEnd();
            if (Trimmed.IsEmpty())
            {
                continue;
            }

            // Format: "type name" or "object<ClassName> name"
            // Find the last space that separates type from name
            int32 LastSpace = FindLastWhitespaceOutsideNameToken(Trimmed);
            if (LastSpace == INDEX_NONE)
            {
                AddError(OutErrors, LineNum, FString::Printf(TEXT("Invalid parameter format: '%s'"), *Trimmed));
                continue;
            }

            FBpirEntryBlock::FParam Param;
            const FString TypeSrc = Trimmed.Left(LastSpace).TrimEnd();
            Param.Name = UnwrapNameTokenOrLegacy(Trimmed.Mid(LastSpace + 1).TrimStart());
            FString ParseErr;
            int32 ParseErrCol = INDEX_NONE;
            if (!BpirTypeSpecParser::ParseTypeSpec(TypeSrc, Param.Type, ParseErr, ParseErrCol))
            {
                // Continue processing remaining params so authors see all errors at once.
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Invalid parameter type '%s' in '%s': %s"),
                        *TypeSrc, *Trimmed,
                        *BpirTypeSpecParser::FormatTypeSpecErrorDetail(ParseErr, ParseErrCol)));
                continue;
            }
            OutBlock.Params.Add(MoveTemp(Param));
        }
    }

    // Check for return type: -> ReturnType
    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    if (OutBlock.Kind == EBpirEntryKind::InputAction)
    {
        if (AfterParens.StartsWith(TEXT("->")))
        {
            AddError(OutErrors, LineNum, TEXT("input_action entries do not declare return types"));
            return false;
        }

        if (!AfterParens.IsEmpty())
        {
            if (!AfterParens.StartsWith(TEXT("[")))
            {
                AddError(OutErrors, LineNum, TEXT("Unexpected text after input_action signature"));
                return false;
            }

            const int32 BracketClose = FindMatchingBracket(AfterParens, 0);
            if (BracketClose == INDEX_NONE)
            {
                AddError(OutErrors, LineNum, TEXT("Unmatched '[' in input_action exec map"));
                return false;
            }
            if (!AfterParens.Mid(BracketClose + 1).TrimStartAndEnd().IsEmpty())
            {
                AddError(OutErrors, LineNum, TEXT("Unexpected text after input_action exec map"));
                return false;
            }

            const FString ExecMap = AfterParens.Mid(1, BracketClose - 1).TrimStartAndEnd();
            if (ExecMap.IsEmpty())
            {
                AddError(OutErrors, LineNum, TEXT("input_action exec map may not be empty"));
                return false;
            }

            OutBlock.EntryExecTargets = ParseExecClause(ExecMap, LineNum, &OutErrors);
            TSet<FString> SeenEventNames;
            for (const FBpirExecTarget& Target : OutBlock.EntryExecTargets)
            {
                if (!BpirSharedConstants::EnhancedInput::IsEventPinName(Target.PinName))
                {
                    AddError(OutErrors, LineNum, FString::Printf(
                        TEXT("Unknown input_action event '%s'"), *Target.PinName));
                }
                if (SeenEventNames.Contains(Target.PinName))
                {
                    AddError(OutErrors, LineNum, FString::Printf(
                        TEXT("Duplicate input_action event '%s'"), *Target.PinName));
                }
                SeenEventNames.Add(Target.PinName);
                if (Target.Label.IsEmpty())
                {
                    AddError(OutErrors, LineNum, FString::Printf(
                        TEXT("input_action event '%s' requires a non-empty label"), *Target.PinName));
                }
            }
            if (OutBlock.EntryExecTargets.IsEmpty())
            {
                AddError(OutErrors, LineNum, TEXT("input_action exec map contains no valid entries"));
            }
        }
        return true;
    }

    if (AfterParens.StartsWith(TEXT("->")))
    {
        FString ReturnStr = AfterParens.Mid(2).TrimStart();

        if ((OutBlock.Kind == EBpirEntryKind::Macro
            || OutBlock.Kind == EBpirEntryKind::Function
            || OutBlock.Kind == EBpirEntryKind::Override) && ReturnStr.StartsWith(TEXT("(")))
        {
            // Parse multi-output: -> (type Name, type Name, ...)
            int32 OutParenClose = FindMatchingParen(ReturnStr, 0);
            if (OutParenClose == INDEX_NONE)
            {
                AddError(OutErrors, LineNum, TEXT("Unmatched '(' in macro output list"));
                return false;
            }
            FString OutputsStr = ReturnStr.Mid(1, OutParenClose - 1).TrimStartAndEnd();
            if (!OutputsStr.IsEmpty())
            {
                TArray<FString> OutputParts = SmartSplit(OutputsStr, TEXT(','));
                for (const FString& Part : OutputParts)
                {
                    FString Trimmed = Part.TrimStartAndEnd();
                    if (Trimmed.IsEmpty()) continue;
                    int32 LastSpace = FindLastWhitespaceOutsideNameToken(Trimmed);
                    if (LastSpace == INDEX_NONE)
                    {
                        AddError(OutErrors, LineNum, FString::Printf(TEXT("Invalid output parameter format: '%s'"), *Trimmed));
                        continue;
                    }
                    FBpirEntryBlock::FParam Param;
                    const FString OutTypeSrc = Trimmed.Left(LastSpace).TrimEnd();
                    Param.Name = UnwrapNameTokenOrLegacy(Trimmed.Mid(LastSpace + 1).TrimStart());
                    FString ParseErr;
                    int32 ParseErrCol = INDEX_NONE;
                    if (!BpirTypeSpecParser::ParseTypeSpec(OutTypeSrc, Param.Type, ParseErr, ParseErrCol))
                    {
                        AddError(OutErrors, LineNum,
                            FString::Printf(TEXT("Invalid output parameter type '%s' in '%s': %s"),
                                *OutTypeSrc, *Trimmed,
                                *BpirTypeSpecParser::FormatTypeSpecErrorDetail(ParseErr, ParseErrCol)));
                        continue;
                    }
                    OutBlock.OutputParams.Add(MoveTemp(Param));
                }
            }

            // Check for exec targets after outputs: [pin -> @label, ...]
            FString AfterOutputs = ReturnStr.Mid(OutParenClose + 1).TrimStart();
            if (AfterOutputs.StartsWith(TEXT("[")))
            {
                int32 BracketClose = FindMatchingBracket(AfterOutputs, 0);
                if (BracketClose != INDEX_NONE)
                {
                    FString ExecStr = AfterOutputs.Mid(1, BracketClose - 1).TrimStartAndEnd();
                    ExtractExecPinNames(ExecStr, OutBlock.ExecOutputNames);
                }
            }
        }
        else if (OutBlock.Kind == EBpirEntryKind::Macro && ReturnStr.StartsWith(TEXT("[")))
        {
            // Exec-only macro with no data outputs: -> [IsValid -> @valid, ...]
            int32 BracketClose = FindMatchingBracket(ReturnStr, 0);
            if (BracketClose != INDEX_NONE)
            {
                FString ExecStr = ReturnStr.Mid(1, BracketClose - 1).TrimStartAndEnd();
                ExtractExecPinNames(ExecStr, OutBlock.ExecOutputNames);
            }
        }
        else
        {
            FString ParseErr;
            int32 ParseErrCol = INDEX_NONE;
            if (!BpirTypeSpecParser::ParseTypeSpec(ReturnStr, OutBlock.ReturnType, ParseErr, ParseErrCol))
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Invalid return type '%s': %s"), *ReturnStr,
                        *BpirTypeSpecParser::FormatTypeSpecErrorDetail(ParseErr, ParseErrCol)));
                // Leave OutBlock.ReturnType default-constructed so downstream
                // IsEmpty() checks behave as "no return declared".
                OutBlock.ReturnType = FBpirTypeSpec{};
            }
        }
    }

    return true;
}

// ----------------------------------------------------------------------------
// Entry-metadata decorators: @meta(...) / @flags(...)
// ----------------------------------------------------------------------------

// Whitelist of @meta(Key=...) keys. Order is normative — matches the contract.
static const TArray<FString>& GetMetaKeyWhitelist()
{
    static const TArray<FString> Keys = {
        TEXT("Category"),
        TEXT("Tooltip"),
        TEXT("Keywords"),
        TEXT("CompactNodeTitle"),
        TEXT("DeprecationMessage"),
    };
    return Keys;
}

// Whitelist of @flags(Identifier, ...) identifiers.
static const TArray<FString>& GetFlagsIdentifierWhitelist()
{
    static const TArray<FString> Ids = {
        TEXT("Public"),
        TEXT("Protected"),
        TEXT("Private"),
        TEXT("Pure"),
        TEXT("Const"),
        TEXT("Exec"),
        TEXT("CallInEditor"),
        TEXT("ThreadSafe"),
        TEXT("UnsafeDuringActorConstruction"),
        TEXT("Deprecated"),
    };
    return Ids;
}

// Parse a double-quoted decorator-value into its unescaped inner contents.
// Returns true on success; false (with error appended) if the value is not a
// well-formed double-quoted string. Outer quotes are stripped; standard BPIR
// inner-escape rules apply via UnescapeBpirString.
static bool ParseQuotedDecoratorString(const FString& Value, int32 LineNum, const FString& KeyName, FString& OutString, TArray<FCompileError>& OutErrors)
{
    const FString Trimmed = Value.TrimStartAndEnd();
    if (Trimmed.Len() < 2 || !Trimmed.StartsWith(TEXT("\"")) || !Trimmed.EndsWith(TEXT("\"")))
    {
        AddError(OutErrors, LineNum, FString::Printf(
            TEXT("@meta value for '%s' must be a double-quoted string"), *KeyName));
        return false;
    }
    const FString Inner = Trimmed.Mid(1, Trimmed.Len() - 2);
    OutString = BpirStructLiteralUtils::UnescapeBpirString(Inner);
    return true;
}

// Coerce a decoder-parsed payload string into an FText, preserving NSLOCTEXT
// namespace/key when the payload is a `NSLOCTEXT("ns", "key", "display")` form.
// Bare literals become a non-localized FText (sufficient for plain-string
// MetaData fields that don't carry a localization identity).
static FText CoerceDecoratorFText(const FString& Payload)
{
    FText Out;
    FString IgnoredError;
    if (CoerceStringToPersistedFText(Payload, /*ExistingText=*/nullptr, Out, IgnoredError))
    {
        return Out;
    }
    return FText::FromString(Payload);
}

bool FBpirParser::ParseMetaDecorator(const FString& Line, int32 LineNum, FBpirEntryMetadata& OutMeta, TArray<FCompileError>& OutErrors)
{
    // Expect: @meta( <Key=Value, ...> )
    const FString Trimmed = Line.TrimStartAndEnd();
    if (!Trimmed.StartsWith(TEXT("@meta(")) || !Trimmed.EndsWith(TEXT(")")))
    {
        AddError(OutErrors, LineNum, TEXT("Invalid @meta decorator: expected '@meta(Key=Value, ...)'"));
        return false;
    }

    if (OutMeta.bMetaPresent)
    {
        AddError(OutErrors, LineNum, TEXT("Duplicate @meta(...) decorator: only one is allowed per entry"));
        return false;
    }
    if (OutMeta.bFlagsPresent)
    {
        AddError(OutErrors, LineNum, TEXT("@meta(...) must appear before @flags(...) above an entry"));
        return false;
    }

    OutMeta.bMetaPresent = true;

    const int32 OpenIdx = Trimmed.Find(TEXT("("));
    const FString Inner = Trimmed.Mid(OpenIdx + 1, Trimmed.Len() - OpenIdx - 2).TrimStartAndEnd();
    if (Inner.IsEmpty())
    {
        return true;
    }

    TArray<FString> Parts = SmartSplit(Inner, TEXT(','));
    TSet<FString> SeenKeys;
    bool bOk = true;

    for (const FString& Raw : Parts)
    {
        FString Part = Raw.TrimStartAndEnd();
        if (Part.IsEmpty())
        {
            AddError(OutErrors, LineNum, TEXT("Empty entry in @meta(...): trailing comma not allowed"));
            bOk = false;
            continue;
        }

        int32 EqIdx = INDEX_NONE;
        Part.FindChar(TEXT('='), EqIdx);
        if (EqIdx == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, FString::Printf(
                TEXT("Invalid @meta entry '%s': expected 'Key=Value'"), *Part));
            bOk = false;
            continue;
        }

        FString Key = Part.Left(EqIdx).TrimEnd();
        FString Value = Part.Mid(EqIdx + 1).TrimStart();

        if (!GetMetaKeyWhitelist().Contains(Key))
        {
            AddError(OutErrors, LineNum, FString::Printf(
                TEXT("Unknown @meta key '%s'. Valid keys are: %s"),
                *Key, *FString::Join(GetMetaKeyWhitelist(), TEXT(", "))));
            bOk = false;
            continue;
        }

        if (SeenKeys.Contains(Key))
        {
            AddError(OutErrors, LineNum, FString::Printf(
                TEXT("Duplicate @meta key '%s'"), *Key));
            bOk = false;
            continue;
        }
        SeenKeys.Add(Key);

        FString Payload;
        if (!ParseQuotedDecoratorString(Value, LineNum, Key, Payload, OutErrors))
        {
            bOk = false;
            continue;
        }

        if (Key == TEXT("Category"))
        {
            OutMeta.Category = CoerceDecoratorFText(Payload);
        }
        else if (Key == TEXT("Tooltip"))
        {
            OutMeta.Tooltip = CoerceDecoratorFText(Payload);
        }
        else if (Key == TEXT("Keywords"))
        {
            OutMeta.Keywords = CoerceDecoratorFText(Payload);
        }
        else if (Key == TEXT("CompactNodeTitle"))
        {
            OutMeta.CompactNodeTitle = CoerceDecoratorFText(Payload);
        }
        else if (Key == TEXT("DeprecationMessage"))
        {
            OutMeta.DeprecationMessage = Payload;
        }
    }

    return bOk;
}

bool FBpirParser::ParseFlagsDecorator(const FString& Line, int32 LineNum, FBpirEntryMetadata& OutMeta, TArray<FCompileError>& OutErrors)
{
    // Expect: @flags( <Identifier, ...> )
    const FString Trimmed = Line.TrimStartAndEnd();
    if (!Trimmed.StartsWith(TEXT("@flags(")) || !Trimmed.EndsWith(TEXT(")")))
    {
        AddError(OutErrors, LineNum, TEXT("Invalid @flags decorator: expected '@flags(Identifier, ...)'"));
        return false;
    }

    if (OutMeta.bFlagsPresent)
    {
        AddError(OutErrors, LineNum, TEXT("Duplicate @flags(...) decorator: only one is allowed per entry"));
        return false;
    }

    OutMeta.bFlagsPresent = true;

    const int32 OpenIdx = Trimmed.Find(TEXT("("));
    const FString Inner = Trimmed.Mid(OpenIdx + 1, Trimmed.Len() - OpenIdx - 2).TrimStartAndEnd();
    if (Inner.IsEmpty())
    {
        return true;
    }

    TArray<FString> Parts = SmartSplit(Inner, TEXT(','));
    TSet<FString> Seen;
    bool bOk = true;
    bool bSawAccess = false;

    for (const FString& Raw : Parts)
    {
        FString Id = Raw.TrimStartAndEnd();
        if (Id.IsEmpty())
        {
            AddError(OutErrors, LineNum, TEXT("Empty entry in @flags(...): trailing comma not allowed"));
            bOk = false;
            continue;
        }

        if (!GetFlagsIdentifierWhitelist().Contains(Id))
        {
            AddError(OutErrors, LineNum, FString::Printf(
                TEXT("Unknown @flags identifier '%s'. Valid identifiers are: %s"),
                *Id, *FString::Join(GetFlagsIdentifierWhitelist(), TEXT(", "))));
            bOk = false;
            continue;
        }

        if (Seen.Contains(Id))
        {
            AddError(OutErrors, LineNum, FString::Printf(
                TEXT("Duplicate @flags identifier '%s'"), *Id));
            bOk = false;
            continue;
        }
        Seen.Add(Id);

        const bool bIsAccess = (Id == TEXT("Public") || Id == TEXT("Protected") || Id == TEXT("Private"));
        if (bIsAccess)
        {
            if (bSawAccess)
            {
                AddError(OutErrors, LineNum, TEXT("Conflicting access specifiers in @flags(...): only one of Public/Protected/Private is allowed"));
                bOk = false;
                continue;
            }
            bSawAccess = true;
        }

        if (Id == TEXT("Public"))                          { OutMeta.Access = FBpirEntryMetadata::EAccess::Public; }
        else if (Id == TEXT("Protected"))                  { OutMeta.Access = FBpirEntryMetadata::EAccess::Protected; }
        else if (Id == TEXT("Private"))                    { OutMeta.Access = FBpirEntryMetadata::EAccess::Private; }
        else if (Id == TEXT("Pure"))                       { OutMeta.bPure = true; }
        else if (Id == TEXT("Const"))                      { OutMeta.bConst = true; }
        else if (Id == TEXT("Exec"))                       { OutMeta.bExec = true; }
        else if (Id == TEXT("CallInEditor"))               { OutMeta.bCallInEditor = true; }
        else if (Id == TEXT("ThreadSafe"))                 { OutMeta.bThreadSafe = true; }
        else if (Id == TEXT("UnsafeDuringActorConstruction")) { OutMeta.bUnsafeDuringActorConstruction = true; }
        else if (Id == TEXT("Deprecated"))                 { OutMeta.bDeprecated = true; }
    }

    return bOk;
}

void FBpirParser::ValidateMetadataKindRestrictions(const FBpirEntryBlock& Block, int32 LineNum, TArray<FCompileError>& OutErrors)
{
    // Engine events: neither @meta nor @flags accepted (no editable metadata).
    if (Block.Kind == EBpirEntryKind::Event)
    {
        if (Block.Metadata.bMetaPresent || Block.Metadata.bFlagsPresent)
        {
            AddError(OutErrors, LineNum,
                TEXT("@meta(...) and @flags(...) are not accepted on 'entry event' lines (engine-defined events carry no editable metadata)"));
        }
        return;
    }

    // Macros: @flags rejected (UK2Node_Tunnel has no EFunctionFlags bag).
    // bCallInEditor / bThreadSafe / etc. on FKismetUserDeclaredFunctionMetadata
    // don't apply to macros either, but they live under @flags(...) in our
    // surface grammar so rejecting @flags wholesale covers both cases.
    if (Block.Kind == EBpirEntryKind::Macro && Block.Metadata.bFlagsPresent)
    {
        AddError(OutErrors, LineNum,
            TEXT("Macros do not accept @flags decorators (no function-flags or MetaData bool fields apply)"));
    }
}

// ----------------------------------------------------------------------------
// Dispatch tables for ParseInstruction
// ----------------------------------------------------------------------------

// Helper: compute the Rest substring from a line given a keyword token
static FString GetRestAfterToken(const FString& Line, const FIrToken& Token)
{
    return Line.Mid(Token.Position + Token.Text.Len()).TrimStart();
}

bool FBpirParser::IsBpirInstructionKeyword(const FString& Keyword)
{
    int32 IgnoredOpcode = 0;
    return GetBpirGrammar().TryGetOpcode(Keyword, IgnoredOpcode);
}

const TMap<FString, FInstructionParserFn>& FBpirParser::GetTopLevelDispatch()
{
    static const TMap<FString, FInstructionParserFn> Table = {
        { TEXT("call"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Call, P.CurrentInst(), P.CurrentErrors());
        }},
        { BpirSharedConstants::Keywords::Message, [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Call, P.CurrentInst(), P.CurrentErrors());
            P.CurrentInst().bInterfaceMessage = true;
        }},
        { BpirSharedConstants::Keywords::ParentCall, [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Call, P.CurrentInst(), P.CurrentErrors());
            P.CurrentInst().bParentCall = true;
        }},
        { TEXT("pure"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Pure, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("set"), [](FBpirParser& P, const FString& Rest, const FString&) {
            auto& Inst = P.CurrentInst();
            Inst.Opcode = EBpirOpcode::Set;

            // Primary format:  set VarName = Value         (self-member set)
            //                  set $VarName = Value        (self-member set; $ prefix is
            //                                               accepted symmetrically with the
            //                                               $-prefixed RHS read form)
            // Extended format: set $target.VarName = Value (external-target set)
            //                  set %ref.VarName = Value
            int32 EqIdx = Rest.Find(TEXT("="));
            if (EqIdx != INDEX_NONE)
            {
                FString LHS = Rest.Left(EqIdx).TrimEnd();

                // Check for external target: $target.Property or %ref.Property
                if ((LHS.StartsWith(TEXT("$")) || LHS.StartsWith(TEXT("%"))) && LHS.Contains(TEXT(".")))
                {
                    int32 DotIdx = INDEX_NONE;
                    LHS.FindLastChar(TEXT('.'), DotIdx);
                    Inst.TypeArg = LHS.Left(DotIdx);       // "$target", "%ref", or "%ref.PinName"
                    Inst.FunctionName = UnwrapNameTokenOrLegacy(LHS.Mid(DotIdx + 1)); // "PropertyName"
                }
                else
                {
                    // Bare LHS — strip optional leading `$` so authors can mirror the
                    // RHS read form (`$Var`) on the LHS without altering semantics.
                    if (LHS.StartsWith(TEXT("$")))
                    {
                        LHS = LHS.Mid(1);
                    }
                    Inst.FunctionName = UnwrapNameTokenOrLegacy(LHS);
                }

                FBpirArg Arg;
                Arg.Value = StripTrailingComment(Rest.Mid(EqIdx + 1).TrimStart());
                Inst.Args.Add(MoveTemp(Arg));
            }
            else
            {
                // Fallback: set VarName Value (space-separated)
                int32 SpaceIdx = Rest.Find(TEXT(" "));
                if (SpaceIdx != INDEX_NONE)
                {
                    FString LHS = Rest.Left(SpaceIdx);
                    if (LHS.StartsWith(TEXT("$")) && !LHS.Contains(TEXT(".")))
                    {
                        LHS = LHS.Mid(1);
                    }
                    Inst.FunctionName = UnwrapNameTokenOrLegacy(LHS);
                    FBpirArg Arg;
                    Arg.Value = StripTrailingComment(Rest.Mid(SpaceIdx + 1).TrimStart());
                    Inst.Args.Add(MoveTemp(Arg));
                }
                else
                {
                    AddError(P.CurrentErrors(), P.CurrentLineNum, FString::Printf(TEXT("'set %s' has no value — use 'set VarName = Value'"), *Rest));
                }
            }
        }},
        { TEXT("return"), [](FBpirParser& P, const FString& Rest, const FString&) {
            auto& Inst = P.CurrentInst();
            Inst.Opcode = EBpirOpcode::Return;
            FString Trimmed = StripTrailingComment(Rest).TrimStartAndEnd();

            // Check for exit pin name: return [PinName] or return [PinName] (args...)
            if (Trimmed.StartsWith(TEXT("[")))
            {
                int32 BracketClose = FBpirParser::FindMatchingBracket(Trimmed, 0);
                if (BracketClose != INDEX_NONE)
                {
                    Inst.TypeArg = Trimmed.Mid(1, BracketClose - 1).TrimStartAndEnd();
                    Trimmed = Trimmed.Mid(BracketClose + 1).TrimStartAndEnd();
                }
            }

            if (Trimmed.StartsWith(TEXT("(")))
            {
                // Macro return: return (PinName: value, PinName2: value2)
                // or: return [ExitPin] (PinName: value, ...)
                int32 ParenClose = P.FindMatchingParen(Trimmed, 0);
                if (ParenClose != INDEX_NONE)
                {
                    FString ArgsStr = Trimmed.Mid(1, ParenClose - 1);
                    Inst.Args = P.ParseArgs(ArgsStr);
                }
            }
            else if (!Trimmed.IsEmpty())
            {
                // Function return: return %value
                FBpirArg Arg;
                Arg.PinName = TEXT("ReturnValue");
                Arg.Value = Trimmed;
                Inst.Args.Add(MoveTemp(Arg));
            }
        }},
        { BpirSharedConstants::Keywords::End, [](FBpirParser& P, const FString& Rest, const FString&) {
            P.CurrentInst().Opcode = EBpirOpcode::End;
            const FString Trimmed = StripTrailingComment(Rest).TrimStartAndEnd();
            if (!Trimmed.IsEmpty())
            {
                AddError(P.CurrentErrors(), P.CurrentLineNum, TEXT("'end' does not accept arguments"));
            }
        }},
        { TEXT("exec"), [](FBpirParser& P, const FString& Rest, const FString&) {
            auto& Inst = P.CurrentInst();
            Inst.Opcode = EBpirOpcode::ExecGoto;
            // Expect "-> @label" or "-> @label.PinName"
            FString Trimmed = Rest;
            if (Trimmed.StartsWith(TEXT("->")))
            {
                FString Label = Trimmed.Mid(2).TrimStart();
                if (Label.StartsWith(TEXT("@")))
                {
                    Label = Label.Mid(1);
                }
                else
                {
                    AddError(P.CurrentErrors(), P.CurrentLineNum, FString::Printf(TEXT("Exec target label should start with '@': '%s'"), *Label));
                }
                FBpirExecTarget Target;
                Target.PinName = TEXT("exec");
                FString LabelOut;
                FString InputPin;
                if (!SplitLabelAndInputPin(Label.TrimEnd(), LabelOut, InputPin))
                {
                    AddError(P.CurrentErrors(), P.CurrentLineNum, FString::Printf(TEXT("Malformed exec target '%s' (expected 'label' or 'label.PinName')"), *Label));
                    LabelOut = Label.TrimEnd();
                }
                Target.Label = LabelOut;
                Target.TargetInputPinName = InputPin;
                Inst.ExecTargets.Add(MoveTemp(Target));
            }
            else
            {
                AddError(P.CurrentErrors(), P.CurrentLineNum, TEXT("Expected '->' after 'exec'"));
            }
        }},
        { TEXT("latent"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseLatentInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("call_dispatcher"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseDispatcherInstruction(Rest, P.CurrentLineNum, EBpirOpcode::CallDispatcher, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("bind_dispatcher"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseDispatcherInstruction(Rest, P.CurrentLineNum, EBpirOpcode::BindDispatcher, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("unbind_dispatcher"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseDispatcherInstruction(Rest, P.CurrentLineNum, EBpirOpcode::UnbindDispatcher, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("clear_dispatcher"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseClearDispatcherInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("field_notify_subscribe"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseDispatcherInstruction(Rest, P.CurrentLineNum, EBpirOpcode::FieldNotifySubscribe, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("field_notify_unsubscribe"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseDispatcherInstruction(Rest, P.CurrentLineNum, EBpirOpcode::FieldNotifyUnsubscribe, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch_int"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseSwitchIntInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch_string"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseSwitchStringInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch_enum"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseSwitchEnumInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("foreach"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseForeachInstruction(Rest, P.CurrentLineNum, false, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("foreach_break"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseForeachInstruction(Rest, P.CurrentLineNum, true, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("while"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseWhileInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("branch"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseBranchInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("sequence"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseSequenceInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch"), [](FBpirParser& P, const FString& Rest, const FString&) {
            P.ParseSwitchInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("break"), [](FBpirParser& P, const FString& Rest, const FString&) {
            // Top-level break<Type>($v) — no named result. Rest includes <Type>($v).
            P.ParseStructInstruction(Rest, P.CurrentLineNum, false, P.CurrentInst(), P.CurrentErrors());
        }},
    };
    return Table;
}

const TMap<FString, FInstructionParserFn>& FBpirParser::GetAfterEqualsDispatch()
{
    static const TMap<FString, FInstructionParserFn> Table = {
        { TEXT("call"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Call, P.CurrentInst(), P.CurrentErrors());
        }},
        { BpirSharedConstants::Keywords::Message, [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Call, P.CurrentInst(), P.CurrentErrors());
            P.CurrentInst().bInterfaceMessage = true;
        }},
        { BpirSharedConstants::Keywords::ParentCall, [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Call, P.CurrentInst(), P.CurrentErrors());
            P.CurrentInst().bParentCall = true;
        }},
        { TEXT("pure"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseCallInstruction(Rest, P.CurrentLineNum, EBpirOpcode::Pure, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("latent"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseLatentInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("branch"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseBranchInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("foreach"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseForeachInstruction(Rest, P.CurrentLineNum, false, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("foreach_break"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseForeachInstruction(Rest, P.CurrentLineNum, true, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("while"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseWhileInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseSwitchInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch_int"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseSwitchIntInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch_string"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseSwitchStringInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("switch_enum"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseSwitchEnumInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("sequence"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseSequenceInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("cast"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            // Rest includes <Type>(...) — passed directly to ParseCastInstruction
            P.CurrentInst().ResultName = ResultName;
            P.ParseCastInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("subsystem"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseSubsystemInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("select"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseSelectInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("macro"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseMacroInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("timeline"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.ParseTimelineInstruction(Rest, P.CurrentLineNum, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("break"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            // break<Type>(...) — Rest includes <Type>(...)
            P.CurrentInst().ResultName = ResultName;
            P.ParseStructInstruction(Rest, P.CurrentLineNum, false, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("make"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            // make<Type>(...) — Rest includes <Type>(...)
            P.CurrentInst().ResultName = ResultName;
            P.ParseStructInstruction(Rest, P.CurrentLineNum, true, P.CurrentInst(), P.CurrentErrors());
        }},
        { TEXT("make_array"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            auto& Inst = P.CurrentInst();
            Inst.ResultName = ResultName;
            Inst.Opcode = EBpirOpcode::MakeArray;
            int32 ParenOpen = Rest.Find(TEXT("("));
            if (ParenOpen == INDEX_NONE)
            {
                AddError(P.CurrentErrors(), P.CurrentLineNum, TEXT("Expected '(' after 'make_array'"));
                return;
            }
            int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
            if (ParenClose == INDEX_NONE)
            {
                AddError(P.CurrentErrors(), P.CurrentLineNum, TEXT("Unmatched '(' in make_array"));
                return;
            }
            FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
            Inst.Args = P.ParseArgs(ArgsStr);
        }},
        { TEXT("self"), [](FBpirParser& P, const FString&, const FString& ResultName) {
            P.CurrentInst().ResultName = ResultName;
            P.CurrentInst().Opcode = EBpirOpcode::Self;
        }},
        { TEXT("enum"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            auto& Inst = P.CurrentInst();
            Inst.ResultName = ResultName;
            Inst.Opcode = EBpirOpcode::Enum;
            Inst.TypeArg = StripTrailingComment(Rest);
        }},
        { TEXT("get"), [](FBpirParser& P, const FString& Rest, const FString& ResultName) {
            auto& Inst = P.CurrentInst();
            Inst.ResultName = ResultName;
            Inst.Opcode = EBpirOpcode::Get;
            Inst.FunctionName = UnwrapNameTokenOrLegacy(StripTrailingComment(Rest));
        }},
    };
    return Table;
}

// ----------------------------------------------------------------------------
// ParseInstruction — tokenizer-based TMap dispatch
// ----------------------------------------------------------------------------

bool FBpirParser::ParseInstruction(const FString& Line, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.SourceLine = LineNum;
    FString WorkingLine = Line;
    if (WorkingLine.TrimStart().StartsWith(TEXT("#")))
    {
        TArray<FIrToken> CommentTokens = FIrTokenizer::Tokenize(WorkingLine, GetBpirGrammar());
        if (!CommentTokens.IsEmpty() && CommentTokens[0].Type == EIrTokenType::Comment)
        {
            OutInst.AuthoredPosition = FVector2D::ZeroVector;
            OutInst.bHasAuthoredPosition = false;
            OutInst.Opcode = EBpirOpcode::Comment;
            OutInst.FunctionName = CommentTokens[0].Text.Mid(1).TrimStart();
            return true;
        }
    }

    FString PositionError;
    OutInst.AuthoredPosition = FVector2D::ZeroVector;
    OutInst.bHasAuthoredPosition = TryExtractAuthoredPosition(WorkingLine, OutInst.AuthoredPosition, PositionError);
    if (!PositionError.IsEmpty())
    {
        AddError(OutErrors, LineNum, PositionError);
        return false;
    }

    FString EnabledStateError;
    if (!ExtractNodeEnabledStateSuffix(
            WorkingLine,
            OutInst.EnabledState,
            OutInst.bHasEnabledState,
            EnabledStateError))
    {
        AddError(OutErrors, LineNum, EnabledStateError);
        return false;
    }

    TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(WorkingLine, GetBpirGrammar());

    // Skip blank lines
    if (Tokens.IsEmpty())
    {
        return false;
    }

    const FIrToken& FirstToken = Tokens[0];

    // Comment line
    if (FirstToken.Type == EIrTokenType::Comment)
    {
        OutInst.Opcode = EBpirOpcode::Comment;
        OutInst.FunctionName = FirstToken.Text.Mid(1).TrimStart();
        return true;
    }

    // Label definition: @name: (or bare @name for backward compat)
    if (FirstToken.Type == EIrTokenType::LabelDef || FirstToken.Type == EIrTokenType::LabelRef)
    {
        // Token text is "@name", strip the @ prefix
        OutInst.Opcode = EBpirOpcode::Label;
        OutInst.FunctionName = FirstToken.Text.Mid(1);
        return true;
    }

    // Stash references for dispatch lambdas
    CurrentLineNum = LineNum;
    CurrentInstPtr = &OutInst;
    CurrentErrorsPtr = &OutErrors;
    bool bDispatchOk = true;

    // Named result: %name = keyword rest...
    if (FirstToken.Type == EIrTokenType::PercentRef)
    {
        FString ResultName = FirstToken.Text.Mid(1); // strip %

        // Find the Equals token
        int32 EqTokenIdx = INDEX_NONE;
        for (int32 i = 1; i < Tokens.Num(); ++i)
        {
            if (Tokens[i].Type == EIrTokenType::Equals)
            {
                EqTokenIdx = i;
                break;
            }
        }

        if (EqTokenIdx == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Expected '=' after %%name: %s"), *WorkingLine));
            return false;
        }

        // PinTypeToBpirType output never contains '=', so the substring between
        // the Colon and the Equals position is the complete typespec source.
        if (Tokens.Num() > 1 && Tokens[1].Type == EIrTokenType::Colon && EqTokenIdx > 1)
        {
            const int32 TypeStart = Tokens[1].Position + 1;
            const int32 TypeEnd = Tokens[EqTokenIdx].Position;
            FString TypeSrc = WorkingLine.Mid(TypeStart, TypeEnd - TypeStart).TrimStartAndEnd();
            FString ParseErr;
            int32 ParseErrCol = INDEX_NONE;
            FBpirTypeSpec ParsedType;
            if (BpirTypeSpecParser::ParseTypeSpec(TypeSrc, ParsedType, ParseErr, ParseErrCol))
            {
                OutInst.DeclaredResultType = MoveTemp(ParsedType);
                OutInst.bHasDeclaredResultType = true;
            }
            else
            {
                UE_LOG(LogBpirCompiler, Warning,
                    TEXT("BPIR L%d: ignoring invalid register-binding type annotation '%s': %s"),
                    LineNum, *TypeSrc,
                    *BpirTypeSpecParser::FormatTypeSpecErrorDetail(ParseErr, ParseErrCol));
            }
        }

        // Check for alias assignment: %name = $var or %name = %other
        // If the first non-whitespace token after '=' is a DollarRef or PercentRef
        // and it is NOT immediately followed by a dot (member access), synthesize Alias.
        {
            int32 FirstRefIdx = INDEX_NONE;
            for (int32 i = EqTokenIdx + 1; i < Tokens.Num(); ++i)
            {
                if (Tokens[i].Type == EIrTokenType::DollarRef || Tokens[i].Type == EIrTokenType::PercentRef)
                {
                    FirstRefIdx = i;
                    break;
                }
                // Any keyword/identifier before the ref means this is not an alias — fall through
                if (Tokens[i].Type == EIrTokenType::Keyword || Tokens[i].Type == EIrTokenType::Identifier)
                {
                    break;
                }
            }

            if (FirstRefIdx != INDEX_NONE)
            {
                // Check the next token: if it is a dot (Unknown token with text "."),
                // this is member access — not supported inline. Fall through to the
                // normal keyword-dispatch path to produce the original error.
                int32 NextIdx = FirstRefIdx + 1;
                bool bHasDotSuffix = false;
                if (NextIdx < Tokens.Num()
                    && Tokens[NextIdx].Type == EIrTokenType::Unknown
                    && Tokens[NextIdx].Text == TEXT("."))
                {
                    bHasDotSuffix = true;
                }

                if (!bHasDotSuffix)
                {
                    // Synthesize an Alias instruction. RHS ref text goes into AliasRhs.
                    OutInst.Opcode = EBpirOpcode::Alias;
                    OutInst.ResultName = ResultName;
                    OutInst.AliasRhs = Tokens[FirstRefIdx].Text; // e.g. "$VarName" or "%other"
                    CurrentInstPtr = nullptr;
                    CurrentErrorsPtr = nullptr;
                    return true;
                }
            }
        }

        // Find keyword token after Equals
        int32 KeywordIdx = INDEX_NONE;
        for (int32 i = EqTokenIdx + 1; i < Tokens.Num(); ++i)
        {
            if (Tokens[i].Type == EIrTokenType::Keyword || Tokens[i].Type == EIrTokenType::Identifier)
            {
                KeywordIdx = i;
                break;
            }
        }

        if (KeywordIdx == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Expected keyword after '=': %s"), *WorkingLine));
            return false;
        }

        const FIrToken& KeywordToken = Tokens[KeywordIdx];
        FString Keyword = KeywordToken.Text.ToLower();

        if (!IsBpirInstructionKeyword(Keyword))
        {
            FString AfterEq = WorkingLine.Mid(Tokens[EqTokenIdx].Position + 1).TrimStart();
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Unknown instruction keyword after '=': %s"), *AfterEq));
            CurrentInstPtr = nullptr;
            CurrentErrorsPtr = nullptr;
            return false;
        }

        // Compute Rest: everything after the keyword token in the original line
        FString Rest = WorkingLine.Mid(KeywordToken.Position + KeywordToken.Text.Len()).TrimStart();

        const auto& DispatchTable = GetAfterEqualsDispatch();
        if (const FInstructionParserFn* Fn = DispatchTable.Find(Keyword))
        {
            int32 ErrorsBefore = OutErrors.Num();
            (*Fn)(*this, Rest, ResultName);
            if (OutErrors.Num() > ErrorsBefore)
            {
                bDispatchOk = false;
            }
        }
        else
        {
            FString AfterEq = WorkingLine.Mid(Tokens[EqTokenIdx].Position + 1).TrimStart();
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Unknown instruction keyword after '=': %s"), *AfterEq));
            bDispatchOk = false;
        }

        CurrentInstPtr = nullptr;
        CurrentErrorsPtr = nullptr;
        return bDispatchOk;
    }

    // Top-level keyword dispatch
    if (FirstToken.Type == EIrTokenType::Keyword && IsBpirInstructionKeyword(FirstToken.Text))
    {
        FString Keyword = FirstToken.Text.ToLower();
        FString Rest = GetRestAfterToken(WorkingLine, FirstToken);

        const auto& DispatchTable = GetTopLevelDispatch();
        if (const FInstructionParserFn* Fn = DispatchTable.Find(Keyword))
        {
            int32 ErrorsBefore = OutErrors.Num();
            (*Fn)(*this, Rest, FString());
            if (OutErrors.Num() > ErrorsBefore)
            {
                bDispatchOk = false;
            }
        }
        else
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Unrecognized instruction: %s"), *WorkingLine));
            bDispatchOk = false;
        }

        CurrentInstPtr = nullptr;
        CurrentErrorsPtr = nullptr;
        return bDispatchOk;
    }

    AddError(OutErrors, LineNum, FString::Printf(TEXT("Unrecognized instruction: %s"), *WorkingLine));
    return false;
}

// ----------------------------------------------------------------------------
// ParseCallInstruction — handles both Call and Pure opcodes
// Format: FunctionName(args) [exec clause]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseCallInstruction(const FString& Rest, int32 LineNum, EBpirOpcode Opcode, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = Opcode;

    // When the function name is backtick-quoted (e.g. `Set Error`(...)), find the paren
    // AFTER the closing backtick so we don't split on spaces inside the name.
    int32 ParenOpen = INDEX_NONE;
    if (Rest.StartsWith(TEXT("`")))
    {
        int32 CloseBacktick = Rest.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
        if (CloseBacktick != INDEX_NONE)
        {
            // Search for '(' starting after the closing backtick (skip optional whitespace)
            ParenOpen = Rest.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, CloseBacktick + 1);
        }
    }
    if (ParenOpen == INDEX_NONE)
    {
        ParenOpen = Rest.Find(TEXT("("));
    }

    if (ParenOpen == INDEX_NONE)
    {
        // No parens — just a function name (rare but valid)
        OutInst.FunctionName = Rest.TrimEnd();
        return true;
    }

    const FString RawNameToken = Rest.Left(ParenOpen).TrimEnd();
    OutInst.FunctionName = UnwrapNameTokenOrLegacy(RawNameToken);

    // Optional qualified `ClassName::MethodName` syntax. The disambiguator is parsed
    // only when the raw token is not backtick-wrapped (a literal-name request
    // always wins, even if its contents contain `::`). Multiple `::` are
    // rejected — there is no nested class namespace in the resolution model.
    if (!RawNameToken.IsEmpty() && RawNameToken[0] != TEXT('`'))
    {
        int32 FirstSep = INDEX_NONE;
        OutInst.FunctionName.FindChar(TEXT(':'), FirstSep);
        if (FirstSep != INDEX_NONE
            && FirstSep + 1 < OutInst.FunctionName.Len()
            && OutInst.FunctionName[FirstSep + 1] == TEXT(':'))
        {
            int32 LastSep = OutInst.FunctionName.Find(BpirSharedConstants::Syntax::QualifiedNameSeparator, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            const FString LeftPart = OutInst.FunctionName.Left(LastSep);
            const FString RightPart = OutInst.FunctionName.Mid(LastSep + 2);

            int32 EarlierSep = INDEX_NONE;
            if (LastSep > 0)
            {
                EarlierSep = LeftPart.Find(BpirSharedConstants::Syntax::QualifiedNameSeparator, ESearchCase::CaseSensitive, ESearchDir::FromStart);
            }

            bool bLeftWellFormed = !LeftPart.IsEmpty();
            for (TCHAR C : LeftPart)
            {
                if (FChar::IsWhitespace(C))
                {
                    bLeftWellFormed = false;
                    break;
                }
            }

            if (!bLeftWellFormed || RightPart.IsEmpty() || EarlierSep != INDEX_NONE)
            {
                AddError(OutErrors, LineNum, FString::Printf(
                    TEXT("Malformed qualified call '%s': expected exactly one 'ClassName::MethodName' separator with non-empty identifiers on both sides"),
                    *OutInst.FunctionName));
                return false;
            }

            OutInst.TypeArg = LeftPart;
            OutInst.FunctionName = RightPart;
        }
    }

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unmatched '(' in call to '%s'"), *OutInst.FunctionName));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    // Optional `node_props { Key: Value, ... }` block between args and exec clause.
    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    const int32 KeywordLen = FCString::Strlen(BpirSharedConstants::Keywords::NodeProps);
    const bool bHasNodePropsKeyword =
        AfterParens.StartsWith(BpirSharedConstants::Keywords::NodeProps)
        && (AfterParens.Len() == KeywordLen
            || FChar::IsWhitespace(AfterParens[KeywordLen])
            || AfterParens[KeywordLen] == TEXT('{'));
    if (bHasNodePropsKeyword)
    {
        FString AfterKeyword = AfterParens.Mid(KeywordLen).TrimStart();
        if (AfterKeyword.IsEmpty() || AfterKeyword[0] != TEXT('{'))
        {
            AddError(OutErrors, LineNum, TEXT("Expected '{' after 'node_props'"));
            return false;
        }

        const bool bIsK2NodeFallback =
            OutInst.FunctionName.StartsWith(TEXT("K2Node_"))
            || OutInst.FunctionName.StartsWith(TEXT("UK2Node_"));
        if (!bIsK2NodeFallback)
        {
            AddError(OutErrors, LineNum, FString::Printf(
                TEXT("'node_props { ... }' is only valid on K2Node fallback calls (function name must start with 'K2Node_' or 'UK2Node_'); got '%s'"),
                *OutInst.FunctionName));
            return false;
        }

        const int32 BraceClose = FBpirParser::FindMatchingBrace(AfterKeyword, 0);
        if (BraceClose == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, TEXT("Unmatched '{' in node_props block"));
            return false;
        }

        const FString PropsBody = AfterKeyword.Mid(1, BraceClose - 1);
        const TArray<FBpirArg> PropPairs = ParseArgs(PropsBody);
        for (const FBpirArg& Pair : PropPairs)
        {
            OutInst.NodeProps.Add(Pair.PinName, Pair.Value);
        }

        AfterParens = AfterKeyword.Mid(BraceClose + 1).TrimStart();
    }

    // Check for exec clause [...]
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseBranchInstruction
// Format: branch(Condition: %val) [true -> @then, false -> @else]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseBranchInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Branch;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '(' after 'branch'"));
        return false;
    }

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '(' in branch"));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    // Parse exec clause
    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseForeachInstruction
// Format: foreach(Array: $items) [body -> @loop, completed -> @done]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseForeachInstruction(const FString& Rest, int32 LineNum, bool bWithBreak, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = bWithBreak ? EBpirOpcode::ForeachBreak : EBpirOpcode::Foreach;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '(' after 'foreach'"));
        return false;
    }

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '(' in foreach"));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseWhileInstruction
// Format: while(Condition: %cond) [body -> @loop, completed -> @done]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseWhileInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::While;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '(' after 'while'"));
        return false;
    }

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '(' in while"));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseSwitchInstruction
// Format: switch(Selection: $val) [0 -> @case0, 1 -> @case1, default -> @def]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseSwitchInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Switch;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '(' after 'switch'"));
        return false;
    }

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '(' in switch"));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseSwitchIntInstruction
// Format: switch_int(Selection: $val) [0 -> @case0, 1 -> @case1, default -> @def]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseSwitchIntInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    bool bResult = ParseSwitchInstruction(Rest, LineNum, OutInst, OutErrors);
    OutInst.Opcode = EBpirOpcode::SwitchInt;
    return bResult;
}

// ----------------------------------------------------------------------------
// ParseSwitchStringInstruction
// Format: switch_string(Selection: $val) ["a" -> @caseA, "b" -> @caseB, default -> @def]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseSwitchStringInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    bool bResult = ParseSwitchInstruction(Rest, LineNum, OutInst, OutErrors);
    OutInst.Opcode = EBpirOpcode::SwitchString;
    return bResult;
}

// ----------------------------------------------------------------------------
// ParseSwitchEnumInstruction
// Format: switch_enum<EnumType>(Selection: $val) [Val0 -> @case0, Val1 -> @case1, default -> @def]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseSwitchEnumInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    // Parse <EnumType> before delegating to the switch parser
    int32 OpenAngle = Rest.Find(TEXT("<"));
    int32 CloseAngle = (OpenAngle != INDEX_NONE) ? FindMatchingAngle(Rest, OpenAngle) : INDEX_NONE;
    if (CloseAngle == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '<EnumType>' after 'switch_enum'"));
        return false;
    }

    FString EnumType = Rest.Mid(OpenAngle + 1, CloseAngle - OpenAngle - 1).TrimStartAndEnd();
    FString AfterType = Rest.Mid(CloseAngle + 1);

    bool bResult = ParseSwitchInstruction(AfterType, LineNum, OutInst, OutErrors);
    OutInst.Opcode = EBpirOpcode::SwitchEnum;
    OutInst.TypeArg = EnumType;
    return bResult;
}

// ----------------------------------------------------------------------------
// ParseSequenceInstruction
// Format: sequence(N) [0 -> @first, 1 -> @second, ...]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseSequenceInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Sequence;

    FString Trimmed = Rest.TrimStart();

    // Parse (N)
    if (Trimmed.StartsWith(TEXT("(")))
    {
        int32 ParenClose = FindMatchingParen(Trimmed, 0);
        if (ParenClose == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, TEXT("Unmatched '(' in sequence"));
            return false;
        }

        FString CountStr = Trimmed.Mid(1, ParenClose - 1).TrimStartAndEnd();

        // Validate count is a positive integer
        bool bIsNumeric = !CountStr.IsEmpty();
        for (int32 ci = 0; ci < CountStr.Len(); ++ci)
        {
            if (!FChar::IsDigit(CountStr[ci]))
            {
                bIsNumeric = false;
                break;
            }
        }
        if (!bIsNumeric)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("sequence count must be a positive integer, got '%s'"), *CountStr));
            return false;
        }

        OutInst.SequenceCount = FCString::Atoi(*CountStr);
        if (OutInst.SequenceCount <= 0)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("sequence count must be > 0, got %d"), OutInst.SequenceCount));
            return false;
        }

        FString AfterParens = Trimmed.Mid(ParenClose + 1).TrimStart();
        ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);
    }

    return true;
}

// ----------------------------------------------------------------------------
// ParseCastInstruction
// Format: <Type>(Object: $obj) [success -> @ok, fail -> @fail]
// Caller already stripped "cast", so Rest starts with "<Type>(...)"
// ----------------------------------------------------------------------------

bool FBpirParser::ParseCastInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Cast;

    // Rest starts with "<Type>(...)"
    int32 OpenAngle = Rest.Find(TEXT("<"));
    int32 CloseAngle = (OpenAngle != INDEX_NONE) ? FindMatchingAngle(Rest, OpenAngle) : INDEX_NONE;
    if (CloseAngle == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '>' in cast<Type>"));
        return false;
    }

    // Extract type between angle brackets
    OutInst.TypeArg = Rest.Mid(OpenAngle + 1, CloseAngle - OpenAngle - 1).TrimStartAndEnd();

    FString AfterType = Rest.Mid(CloseAngle + 1).TrimStart();

    int32 ParenOpen = AfterType.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '(' after cast<Type>"));
        return false;
    }

    int32 ParenClose = FindMatchingParen(AfterType, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '(' in cast"));
        return false;
    }

    FString ArgsStr = AfterType.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    if (OutInst.Args.Num() == 0)
    {
        AddError(OutErrors, LineNum, TEXT("cast requires an object input argument"));
        return false;
    }

    FString AfterParens = AfterType.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseSelectInstruction
// Format: select(Condition: %cond, A: $a, B: $b)
// ----------------------------------------------------------------------------

bool FBpirParser::ParseSelectInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Select;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '(' after 'select'"));
        return false;
    }

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '(' in select"));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    return true;
}

// ----------------------------------------------------------------------------
// ParseMacroInstruction
// Format: macro MacroName(args) [exec targets]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseMacroInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Macro;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        // No parens — just a macro name
        OutInst.FunctionName = Rest.TrimEnd();
        return true;
    }

    OutInst.FunctionName = Rest.Left(ParenOpen).TrimEnd();

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unmatched '(' in macro '%s'"), *OutInst.FunctionName));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseLatentInstruction
// Format: LatentFunctionName(args) [completed -> @done]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseLatentInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Latent;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        OutInst.FunctionName = Rest.TrimEnd();
        return true;
    }

    OutInst.FunctionName = Rest.Left(ParenOpen).TrimEnd();

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unmatched '(' in latent '%s'"), *OutInst.FunctionName));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseTimelineInstruction
// Format: timeline TimelineName(args) [update -> @upd, finished -> @fin]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseTimelineInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Timeline;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        OutInst.FunctionName = Rest.TrimEnd();
        return true;
    }

    OutInst.FunctionName = Rest.Left(ParenOpen).TrimEnd();

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unmatched '(' in timeline '%s'"), *OutInst.FunctionName));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseStructInstruction
// Format for make: <Type>(args)
// Format for break: <Type>(args)
// Caller already stripped "make" or "break", so Rest starts with "<Type>(...)"
// ----------------------------------------------------------------------------

bool FBpirParser::ParseStructInstruction(const FString& Rest, int32 LineNum, bool bMake, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = bMake ? EBpirOpcode::MakeStruct : EBpirOpcode::BreakStruct;

    // Rest starts with "<Type>(...)"
    int32 OpenAngle = Rest.Find(TEXT("<"));
    int32 CloseAngle = (OpenAngle != INDEX_NONE) ? FindMatchingAngle(Rest, OpenAngle) : INDEX_NONE;
    if (CloseAngle == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Expected '>' in %s<Type>"), bMake ? TEXT("make") : TEXT("break")));
        return false;
    }

    // Extract type between angle brackets
    OutInst.TypeArg = Rest.Mid(OpenAngle + 1, CloseAngle - OpenAngle - 1).TrimStartAndEnd();

    FString AfterType = Rest.Mid(CloseAngle + 1).TrimStart();

    int32 ParenOpen = AfterType.Find(TEXT("("));
    if (ParenOpen != INDEX_NONE)
    {
        int32 ParenClose = FindMatchingParen(AfterType, ParenOpen);
        if (ParenClose == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, FString::Printf(TEXT("Unmatched '(' in %s"), bMake ? TEXT("make") : TEXT("break")));
            return false;
        }

        FString ArgsStr = AfterType.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
        OutInst.Args = ParseArgs(ArgsStr);
    }

    return true;
}

// ----------------------------------------------------------------------------
// ParseDispatcherInstruction
// Format: DispatcherName(args) [exec targets]
// ----------------------------------------------------------------------------

bool FBpirParser::ParseDispatcherInstruction(const FString& Rest, int32 LineNum, EBpirOpcode Opcode, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = Opcode;

    int32 ParenOpen = Rest.Find(TEXT("("));
    if (ParenOpen == INDEX_NONE)
    {
        OutInst.FunctionName = Rest.TrimEnd();
        return true;
    }

    OutInst.FunctionName = Rest.Left(ParenOpen).TrimEnd();

    int32 ParenClose = FindMatchingParen(Rest, ParenOpen);
    if (ParenClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, FString::Printf(TEXT("Unmatched '(' in dispatcher '%s'"), *OutInst.FunctionName));
        return false;
    }

    FString ArgsStr = Rest.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
    OutInst.Args = ParseArgs(ArgsStr);

    FString AfterParens = Rest.Mid(ParenClose + 1).TrimStart();
    ParseExecClauseFromSuffix(AfterParens, LineNum, OutInst, OutErrors);

    return true;
}

// ----------------------------------------------------------------------------
// ParseClearDispatcherInstruction
// Format: clear_dispatcher DispatcherName
// ----------------------------------------------------------------------------

bool FBpirParser::ParseClearDispatcherInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    return ParseDispatcherInstruction(Rest, LineNum, EBpirOpcode::ClearDispatcher, OutInst, OutErrors);
}

// ----------------------------------------------------------------------------
// ParseSubsystemInstruction
// Format: <Type>()   or   <Type>(args...)
// Caller already stripped "subsystem", so Rest starts with "<Type>(...)"
// ----------------------------------------------------------------------------

bool FBpirParser::ParseSubsystemInstruction(const FString& Rest, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    OutInst.Opcode = EBpirOpcode::Subsystem;

    int32 OpenAngle = Rest.Find(TEXT("<"));
    int32 CloseAngle = (OpenAngle != INDEX_NONE) ? FindMatchingAngle(Rest, OpenAngle) : INDEX_NONE;
    if (CloseAngle == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Expected '>' in subsystem<Type>"));
        return false;
    }

    OutInst.TypeArg = Rest.Mid(OpenAngle + 1, CloseAngle - OpenAngle - 1).TrimStartAndEnd();

    FString AfterType = Rest.Mid(CloseAngle + 1).TrimStart();

    int32 ParenOpen = AfterType.Find(TEXT("("));
    if (ParenOpen != INDEX_NONE)
    {
        int32 ParenClose = FindMatchingParen(AfterType, ParenOpen);
        if (ParenClose == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, TEXT("Unmatched '(' in subsystem"));
            return false;
        }

        FString ArgsStr = AfterType.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
        if (!ArgsStr.TrimStartAndEnd().IsEmpty())
        {
            OutInst.Args = ParseArgs(ArgsStr);
        }
    }

    return true;
}

// ----------------------------------------------------------------------------
// ParseExecClauseFromSuffix
// ----------------------------------------------------------------------------

bool FBpirParser::ParseExecClauseFromSuffix(const FString& AfterParens, int32 LineNum, FBpirInstruction& OutInst, TArray<FCompileError>& OutErrors)
{
    FString Trimmed = AfterParens.TrimStart();
    if (!Trimmed.StartsWith(TEXT("[")))
    {
        return true; // No exec clause present
    }

    int32 BracketClose = FindMatchingBracket(Trimmed, 0);
    if (BracketClose == INDEX_NONE)
    {
        AddError(OutErrors, LineNum, TEXT("Unmatched '[' in exec clause"));
        return false;
    }

    FString ClauseStr = Trimmed.Mid(1, BracketClose - 1);
    OutInst.ExecTargets = ParseExecClause(ClauseStr, LineNum, &OutErrors);
    return true;
}

// ----------------------------------------------------------------------------
// ParseArgs
// Input: "PinName: Value, PinName2: Value2"
// ----------------------------------------------------------------------------

TArray<FBpirArg> FBpirParser::ParseArgs(const FString& ArgsStr)
{
    TArray<FBpirArg> Result;

    FString Trimmed = ArgsStr.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return Result;
    }

    TArray<FString> Parts = SmartSplit(Trimmed, TEXT(','));

    for (const FString& Part : Parts)
    {
        FString P = Part.TrimStartAndEnd();
        if (P.IsEmpty())
        {
            continue;
        }

        FBpirArg Arg;

        // Find first ':' that is not inside angle brackets or parens
        int32 ColonIdx = INDEX_NONE;
        int32 Depth = 0;
        int32 AngleDepth = 0;
        bool bInQuote = false;
        bool bInName = false;

        for (int32 i = 0; i < P.Len(); ++i)
        {
            TCHAR Ch = P[i];

            if (!bInName && Ch == TEXT('"') && IsUnescapedQuote(P, i))
            {
                bInQuote = !bInQuote;
                continue;
            }

            if (!bInQuote && Ch == TEXT('`') && FIrTextUtils::IsUnescapedBacktick(P, i))
            {
                bInName = !bInName;
                continue;
            }

            if (bInQuote || bInName)
            {
                continue;
            }

            if (Ch == TEXT('('))
            {
                ++Depth;
            }
            else if (Ch == TEXT(')'))
            {
                --Depth;
            }
            else if (Ch == TEXT('<'))
            {
                ++AngleDepth;
            }
            else if (Ch == TEXT('>'))
            {
                --AngleDepth;
            }
            else if (Ch == TEXT(':') && Depth == 0 && AngleDepth == 0)
            {
                const bool bPartOfScopeOperator =
                    (i > 0 && P[i - 1] == TEXT(':')) ||
                    (i + 1 < P.Len() && P[i + 1] == TEXT(':'));
                if (bPartOfScopeOperator)
                {
                    continue;
                }
                ColonIdx = i;
                break;
            }
        }

        if (ColonIdx != INDEX_NONE)
        {
            bool bWasNameToken = false;
            const FString PinName = UnwrapNameTokenOrLegacy(P.Left(ColonIdx).TrimEnd(), &bWasNameToken);
            Arg.PinName = bWasNameToken ? PinName : NormalizePinName(PinName);
            Arg.Value = P.Mid(ColonIdx + 1).TrimStart();
        }
        else
        {
            // Positional argument (no pin name)
            Arg.Value = P;
        }

        Result.Add(MoveTemp(Arg));
    }

    return Result;
}

// ----------------------------------------------------------------------------
// ParseExecClause
// Input: "true -> @then, false -> @else"
// ----------------------------------------------------------------------------

TArray<FBpirExecTarget> FBpirParser::ParseExecClause(const FString& ClauseStr, int32 LineNum, TArray<FCompileError>* OutErrors)
{
    TArray<FBpirExecTarget> Result;

    FString Trimmed = ClauseStr.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return Result;
    }

    TArray<FString> Parts = SmartSplit(Trimmed, TEXT(','));

    for (const FString& Part : Parts)
    {
        FString P = Part.TrimStartAndEnd();
        if (P.IsEmpty())
        {
            continue;
        }

        // Format: "PinName -> @LabelName"
        int32 ArrowIdx = P.Find(TEXT("->"));
        if (ArrowIdx == INDEX_NONE)
        {
            if (OutErrors)
            {
                AddError(*OutErrors, LineNum, FString::Printf(TEXT("Expected '->' in exec clause entry: '%s'"), *P));
            }
            continue;
        }

        FBpirExecTarget Target;
        Target.PinName = P.Left(ArrowIdx).TrimEnd();

        FString LabelPart = P.Mid(ArrowIdx + 2).TrimStart();
        if (LabelPart.StartsWith(TEXT("@")))
        {
            LabelPart = LabelPart.Mid(1);
        }
        else
        {
            if (OutErrors)
            {
                AddError(*OutErrors, LineNum, FString::Printf(TEXT("Exec target label should start with '@': '%s'"), *LabelPart));
            }
        }

        FString LabelOut;
        FString InputPin;
        if (!SplitLabelAndInputPin(LabelPart.TrimEnd(), LabelOut, InputPin))
        {
            if (OutErrors)
            {
                AddError(*OutErrors, LineNum, FString::Printf(TEXT("Malformed exec target '%s' (expected 'label' or 'label.PinName')"), *LabelPart));
            }
            LabelOut = LabelPart.TrimEnd();
        }
        Target.Label = LabelOut;
        Target.TargetInputPinName = InputPin;

        Result.Add(MoveTemp(Target));
    }

    return Result;
}

// ----------------------------------------------------------------------------
// BuildIndicesAndValidate
// ----------------------------------------------------------------------------

bool FBpirParser::BuildIndicesAndValidate(FBpirEntryBlock& Block, TArray<FCompileError>& OutErrors, bool bSkipReferenceValidation)
{
    int32 InitialErrors = OutErrors.Num();

    // Build LabelIndex
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        const FBpirInstruction& Inst = Block.Instructions[i];
        if (Inst.Opcode == EBpirOpcode::Label)
        {
            if (Block.LabelIndex.Contains(Inst.FunctionName))
            {
                AddError(OutErrors, Inst.SourceLine,
                    FString::Printf(TEXT("Duplicate label: @%s"), *Inst.FunctionName));
            }
            else
            {
                Block.LabelIndex.Add(Inst.FunctionName, i);
            }
        }
    }

    // Build ValueIndex
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        const FBpirInstruction& Inst = Block.Instructions[i];
        if (!Inst.ResultName.IsEmpty())
        {
            if (Block.ValueIndex.Contains(Inst.ResultName))
            {
                AddError(OutErrors, Inst.SourceLine,
                    FString::Printf(TEXT("Duplicate result name: %%%s"), *Inst.ResultName));
            }
            else
            {
                Block.ValueIndex.Add(Inst.ResultName, i);
            }
        }
    }

    if (!bSkipReferenceValidation)
    {
        // Validate %references in args and TypeArg
        for (int32 InstIndex = 0; InstIndex < Block.Instructions.Num(); ++InstIndex)
        {
            const FBpirInstruction& Inst = Block.Instructions[InstIndex];

            // Collect all values to validate: args + TypeArg
            TArray<FString> ValuesToCheck;
            for (const FBpirArg& Arg : Inst.Args)
            {
                ValuesToCheck.Add(Arg.Value);
            }
            if (Inst.TypeArg.StartsWith(TEXT("%")))
            {
                ValuesToCheck.Add(Inst.TypeArg);
            }

            for (const FString& Value : ValuesToCheck)
            {
                if (Value.StartsWith(TEXT("%")))
                {
                    FString RefName = Value.Mid(1);
                    // Strip .PinName suffix for validation
                    int32 DotIdx = INDEX_NONE;
                    if (RefName.FindChar(TEXT('.'), DotIdx))
                    {
                        RefName = RefName.Left(DotIdx);
                    }
                    if (!Block.ValueIndex.Contains(RefName))
                    {
                        AddError(OutErrors, Inst.SourceLine,
                            FString::Printf(TEXT("Undefined value reference: %%%s"), *RefName));
                    }
                    else
                    {
                        int32 DefIndex = Block.ValueIndex[RefName];
                        if (DefIndex > InstIndex)
                        {
                            AddError(OutErrors, Inst.SourceLine,
                                FString::Printf(TEXT("Forward reference to %%%s (defined on a later line)"), *RefName));
                        }
                    }
                }
            }
        }

        // Validate @label references in exec targets
        for (const FBpirInstruction& Inst : Block.Instructions)
        {
            for (const FBpirExecTarget& Target : Inst.ExecTargets)
            {
                if (!Block.LabelIndex.Contains(Target.Label))
                {
                    AddError(OutErrors, Inst.SourceLine,
                        FString::Printf(TEXT("Undefined label reference: @%s"), *Target.Label));
                }
            }
        }

        for (const FBpirExecTarget& Target : Block.EntryExecTargets)
        {
            if (!Block.LabelIndex.Contains(Target.Label))
            {
                AddError(OutErrors, -1,
                    FString::Printf(TEXT("Undefined label reference: @%s"), *Target.Label));
            }
        }
    }

    return OutErrors.Num() == InitialErrors;
}
