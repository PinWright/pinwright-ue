// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRParser.h"

#include "AGIR/AGIRGrammar.h"
#include "IrCore/IrTextUtils.h"
#include "IrCore/IrTokenizer.h"

namespace
{
// Push a parse error and continue. Mirrors MGIR's AddError helper.
void AddError(TArray<FAGIRParseError>& Errors, int32 Line, const FString& Message, const FString& Code = FString())
{
    Errors.Add(FAGIRParseError(Line, Message, Code));
}

// True when a `"` at Index is not preceded by an odd number of backslashes —
// matches the MGIR/BPIR convention so embedded string literals don't confuse
// trailing-suffix scanners.
bool IsUnescapedQuote(const FString& Str, int32 Index)
{
    return FIrTextUtils::IsUnescapedQuote(Str, Index);
}

// Drop everything from the first unquoted `#` onward. AGIR uses the same
// inline-comment convention as BPIR / MGIR.
FString StripTrailingComment(const FString& Value)
{
    return FIrTextUtils::StripTrailingComment(Value);
}

// Locate matching close character for a paired delimiter starting at OpenPos.
// Honours nested same-kind delimiters and skips over quoted regions.
int32 FindMatchingChar(const FString& Str, int32 OpenPos, TCHAR OpenChar, TCHAR CloseChar)
{
    return FIrTextUtils::FindMatchingChar(Str, OpenPos, OpenChar, CloseChar);
}

// Scan top-level (paren/angle/bracket/brace zero, outside quotes) positions
// of a delimiter character. Used to split argument lists and find top-level
// field separators.
TArray<int32> FindTopLevelDelimiterPositions(const FString& Str, TCHAR Delimiter, bool bStopAtFirst = false)
{
    return FIrTextUtils::FindTopLevelDelimiterPositions(Str, Delimiter, bStopAtFirst);
}

// Split on top-level Delimiter occurrences. Empty trailing segment is
// dropped; per-segment trim is applied so callers don't have to.
TArray<FString> SmartSplit(const FString& Str, TCHAR Delimiter)
{
    return FIrTextUtils::SmartSplit(Str, Delimiter);
}

bool ReadLeadingNameToken(FString& InOutText, FString& OutName, FString& OutError)
{
    OutName.Reset();
    OutError.Reset();
    FString Working = InOutText.TrimStart();
    if (Working.IsEmpty())
    {
        OutError = TEXT("Missing name token.");
        return false;
    }

    FString TokenText;
    const TCHAR Delimiter = Working[0];
    if (Delimiter == TEXT('`') || Delimiter == TEXT('"'))
    {
        int32 EndDelimiter = INDEX_NONE;
        for (int32 Index = 1; Index < Working.Len(); ++Index)
        {
            const bool bIsEnd =
                Delimiter == TEXT('`')
                    ? FIrTextUtils::IsUnescapedBacktick(Working, Index)
                    : FIrTextUtils::IsUnescapedQuote(Working, Index);
            if (Working[Index] == Delimiter && bIsEnd)
            {
                EndDelimiter = Index;
                break;
            }
        }
        if (EndDelimiter == INDEX_NONE)
        {
            OutError = Delimiter == TEXT('`')
                ? TEXT("Unterminated backtick-delimited name token.")
                : TEXT("Unterminated string-literal name token.");
            return false;
        }
        TokenText = Working.Left(EndDelimiter + 1);
        Working = Working.Mid(EndDelimiter + 1).TrimStart();
    }
    else
    {
        int32 EndIdx = 0;
        while (EndIdx < Working.Len() && !FChar::IsWhitespace(Working[EndIdx]))
        {
            ++EndIdx;
        }
        TokenText = Working.Left(EndIdx);
        Working = Working.Mid(EndIdx).TrimStart();
    }

    const bool bUnwrapped =
        TokenText.StartsWith(TEXT("\""))
            ? FIrTextUtils::TryUnwrapStringLiteral(TokenText, OutName, OutError)
            : FIrTextUtils::TryUnwrapNameToken(TokenText, OutName, OutError);
    if (!bUnwrapped)
    {
        return false;
    }

    InOutText = Working;
    return true;
}

// Extract trailing `@(x, y)` position annotation if present. Mirrors
// MGIRParser::TryExtractPosition. Mutates InOutLine to drop the marker on
// success. Returns false (no error) when no marker exists.
bool TryExtractPosition(FString& InOutLine, FVector2D& OutPosition, FString& OutError)
{
    return FIrTextUtils::TryExtractPosition(InOutLine, OutPosition, OutError);
}

// Extract trailing `guid="..."` annotation, scanning right-to-left so any
// `guid=` inside argument values is ignored. Mutates InOutLine to drop the
// matched annotation. Returns false silently when no annotation is present.
bool TryExtractGuidAnnotation(FString& InOutLine, FString& OutGuid)
{
    OutGuid.Reset();
    const FString Marker = TEXT("guid=\"");
    bool bInQuote = false;
    int32 MarkerPos = INDEX_NONE;

    for (int32 Index = 0; Index <= InOutLine.Len() - Marker.Len(); ++Index)
    {
        if (InOutLine[Index] == TEXT('"') && IsUnescapedQuote(InOutLine, Index))
        {
            bInQuote = !bInQuote;
            continue;
        }
        if (bInQuote)
        {
            continue;
        }

        // Match must be a token boundary: preceded by whitespace or start.
        if (Index > 0 && !FChar::IsWhitespace(InOutLine[Index - 1]))
        {
            continue;
        }

        if (FCString::Strncmp(*InOutLine + Index, *Marker, Marker.Len()) == 0)
        {
            MarkerPos = Index;
        }
    }

    if (MarkerPos == INDEX_NONE)
    {
        return false;
    }

    const int32 ValueStart = MarkerPos + Marker.Len();
    int32 CloseQuote = INDEX_NONE;
    for (int32 Index = ValueStart; Index < InOutLine.Len(); ++Index)
    {
        if (InOutLine[Index] == TEXT('"') && IsUnescapedQuote(InOutLine, Index))
        {
            CloseQuote = Index;
            break;
        }
    }

    if (CloseQuote == INDEX_NONE)
    {
        return false;
    }

    OutGuid = InOutLine.Mid(ValueStart, CloseQuote - ValueStart);
    InOutLine = (InOutLine.Left(MarkerPos) + InOutLine.Mid(CloseQuote + 1)).TrimStartAndEnd();
    return true;
}

// Returns true if the (already-stripped, already-position/guid-stripped) line
// ends with `{`, indicating an open block. Strips the trailing `{` from
// InOutLine on success.
bool ConsumeTrailingOpenBrace(FString& InOutLine)
{
    FString Trimmed = InOutLine.TrimEnd();
    if (Trimmed.EndsWith(TEXT("{")))
    {
        Trimmed.LeftChopInline(1);
        InOutLine = Trimmed.TrimEnd();
        return true;
    }
    return false;
}

// Turn `Name: value, Name: value` parenthesized field-list text into an
// ordered FAGIRArg array. Bare values remain allowed for positional operands,
// but top-level `=` is rejected in this parenthesized grammar.
bool ParseArgList(const FString& ArgsText, int32 LineNum, TArray<FAGIRParseError>& OutErrors, TArray<FAGIRArg>& OutArgs)
{
    OutArgs.Reset();
    const FString Trimmed = ArgsText.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return true;
    }

    for (const FString& Part : SmartSplit(Trimmed, TEXT(',')))
    {
        const FString Pair = Part.TrimStartAndEnd();
        if (Pair.IsEmpty())
        {
            continue;
        }

        FAGIRArg Arg;
        if (FindTopLevelDelimiterPositions(Pair, TEXT('='), true).Num() > 0)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("AGIR field list uses ':' separators; '=' is not valid in '%s'"), *Pair),
                TEXT("AGIR_BAD_FIELD_LIST"));
            return false;
        }

        const TArray<int32> ColonPositions = FindTopLevelDelimiterPositions(Pair, TEXT(':'), true);
        if (ColonPositions.Num() > 0)
        {
            const int32 ColonIndex = ColonPositions[0];
            Arg.Name = Pair.Left(ColonIndex).TrimStartAndEnd();
            Arg.Value = Pair.Mid(ColonIndex + 1).TrimStartAndEnd();
        }
        else
        {
            Arg.Value = Pair;
        }
        OutArgs.Add(MoveTemp(Arg));
    }

    return true;
}

// Tokenize and apply common per-line annotations (position, guid). On
// position-marker syntax errors, push to OutErrors and return false so the
// caller skips the instruction; missing annotations are not errors.
bool StripAnnotations(FString& InOutLine, int32 LineNum, FAGIRInstruction& OutInst, TArray<FAGIRParseError>& OutErrors)
{
    FString PositionError;
    OutInst.bHasPosition = TryExtractPosition(InOutLine, OutInst.Position, PositionError);
    if (!PositionError.IsEmpty())
    {
        AddError(OutErrors, LineNum, PositionError, TEXT("AGIR_BAD_POSITION"));
        return false;
    }

    TryExtractGuidAnnotation(InOutLine, OutInst.NodeGuid);
    return true;
}

// Re-tokenizes a line and returns the first non-comment token, if any.
bool TryReadFirstToken(const FString& Line, FIrToken& OutToken)
{
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Line, FAGIRGrammar::Get());
    for (const FIrToken& Token : Tokens)
    {
        if (Token.Type == EIrTokenType::Comment)
        {
            return false;
        }
        OutToken = Token;
        return true;
    }
    return false;
}

// Parse `entry anim_graph <name-token> {` opener. Returns false (with error pushed)
// on malformed entries.
bool ParseEntryHeader(const FString& Line, int32 LineNum, FAGIREntryBlock& OutBlock, TArray<FAGIRParseError>& OutErrors)
{
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Line, FAGIRGrammar::Get());
    if (Tokens.Num() < 2)
    {
        AddError(OutErrors, LineNum,
            TEXT("Incomplete AGIR entry line; expected 'entry anim_graph|anim_layer|anim_function <name-token>'"),
            TEXT("AGIR_BAD_BLOCK_HEADER"));
        return false;
    }

    const FString EntryToken = Tokens[0].Text.ToLower();
    if (EntryToken != TEXT("entry"))
    {
        AddError(OutErrors, LineNum, TEXT("AGIR entry line must start with 'entry'"),
            TEXT("AGIR_BAD_BLOCK_HEADER"));
        return false;
    }

    const FString KindToken = Tokens[1].Text.ToLower();
    if (KindToken == TEXT("anim_graph"))
    {
        OutBlock.Kind = EAGIREntryKind::AnimGraph;
    }
    else if (KindToken == TEXT("anim_layer"))
    {
        OutBlock.Kind = EAGIREntryKind::AnimLayer;
    }
    else if (KindToken == TEXT("anim_function"))
    {
        OutBlock.Kind = EAGIREntryKind::AnimFunction;
    }
    else
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Unknown AGIR entry kind: %s"), *Tokens[1].Text),
            TEXT("AGIR_BAD_BLOCK_HEADER"));
        return false;
    }

    FString NameRest = Line.Mid(Tokens[1].Position + Tokens[1].Text.Len()).TrimStartAndEnd();
    FString NameError;
    if (ReadLeadingNameToken(NameRest, OutBlock.Name, NameError))
    {
        return true;
    }

    AddError(OutErrors, LineNum,
        FString::Printf(TEXT("AGIR entry line has invalid block name: %s"), *NameError),
        TEXT("AGIR_BAD_BLOCK_HEADER"));
    return false;
}

// Forward decl: ParseAssignedCall delegates to ParseSingleLineOpcode for the
// Phase 3 cliff opcodes that the decompiler emits in `%n = <op> ...` form.
bool ParseSingleLineOpcode(EAGIROpcode Opcode, const FString& Rest, int32 LineNum,
    FAGIRInstruction& OutInst, TArray<FAGIRParseError>& OutErrors);

// Parse `%name = call <ClassPath> ( ... )` — the assigned generic call form.
// Rest is everything after `=`. Returns false on malformed input (error
// already pushed).
bool ParseAssignedCall(const FString& ResultName, const FString& Rest, int32 LineNum,
    FAGIRInstruction& OutInst, TArray<FAGIRParseError>& OutErrors)
{
    if (ResultName.IsEmpty())
    {
        AddError(OutErrors, LineNum, TEXT("AGIR result name is empty"), TEXT("AGIR_BAD_OPCODE"));
        return false;
    }

    // Read leading `call` keyword.
    FString Working = Rest.TrimStartAndEnd();
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Working, FAGIRGrammar::Get());
    if (Tokens.Num() == 0 || Tokens[0].Type != EIrTokenType::Keyword)
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Expected AGIR opcode after assignment to %%%s"), *ResultName),
            TEXT("AGIR_BAD_OPCODE"));
        return false;
    }

    int32 OpcodeValue = 0;
    if (!FAGIRGrammar::Get().TryGetOpcode(Tokens[0].Text, OpcodeValue))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Unknown AGIR opcode '%s' after %%%s"), *Tokens[0].Text, *ResultName),
            TEXT("AGIR_BAD_OPCODE"));
        return false;
    }

    const EAGIROpcode Opcode = static_cast<EAGIROpcode>(OpcodeValue);

    // Phase 3 cliff opcodes are emitted by the decompiler in the assigned form
    // `%n = <opcode> ...` (see AGIRTextEmitter::EmitBlendSpaceGraph etc.) so
    // round-trip text parses cleanly; the compiler then surfaces the cliff as
    // AGIR_SUBGRAPH_NOT_SUPPORTED. The instruction body after the opcode is the
    // same single-line form ParseSingleLineOpcode handles, so delegate there
    // and stamp the assigned result name afterwards.
    const bool bIsCliffOpcode =
        Opcode == EAGIROpcode::BlendSpace
        || Opcode == EAGIROpcode::LayeredBlend
        || Opcode == EAGIROpcode::LinkedAnim
        || Opcode == EAGIROpcode::LinkedInputPose
        || Opcode == EAGIROpcode::SaveCachedPose
        || Opcode == EAGIROpcode::UseCachedPose;

    if (Opcode != EAGIROpcode::Call && !bIsCliffOpcode)
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Opcode '%s' cannot be assigned to a result; only 'call' supports `%%name = ...`"),
                *Tokens[0].Text),
            TEXT("AGIR_BAD_OPCODE"));
        return false;
    }

    if (bIsCliffOpcode)
    {
        // Decompiler emits `<head> (reflected_fields)` for these opcodes; pull
        // the trailing paren list off so the head can run through the
        // single-line path, then merge the reflected-field args back in.
        FString CliffRest = Working.Mid(Tokens[0].Position + Tokens[0].Text.Len()).TrimStart();
        TArray<FAGIRArg> ReflectedArgs;
        const int32 OpenParen = CliffRest.Find(TEXT("("));
        if (OpenParen != INDEX_NONE)
        {
            const int32 CloseParen = FindMatchingChar(CliffRest, OpenParen, TEXT('('), TEXT(')'));
            if (CloseParen == INDEX_NONE)
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Unmatched '(' in '%s' argument list"), *Tokens[0].Text),
                    TEXT("AGIR_BAD_OPCODE"));
                return false;
            }
            const FString ArgsText = CliffRest.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
            if (!ParseArgList(ArgsText, LineNum, OutErrors, ReflectedArgs))
            {
                return false;
            }
            CliffRest = CliffRest.Left(OpenParen).TrimEnd();
        }

        if (!ParseSingleLineOpcode(Opcode, CliffRest, LineNum, OutInst, OutErrors))
        {
            return false;
        }
        OutInst.ResultName = ResultName;
        OutInst.Args.Append(MoveTemp(ReflectedArgs));
        return true;
    }

    Working = Working.Mid(Tokens[0].Position + Tokens[0].Text.Len()).TrimStart();

    // Class path runs up to the opening paren.
    const int32 OpenParen = Working.Find(TEXT("("));
    FString ClassText;
    FString ArgsText;
    if (OpenParen == INDEX_NONE)
    {
        ClassText = Working.TrimEnd();
    }
    else
    {
        const int32 CloseParen = FindMatchingChar(Working, OpenParen, TEXT('('), TEXT(')'));
        if (CloseParen == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, TEXT("Unmatched '(' in call argument list"),
                TEXT("AGIR_BAD_OPCODE"));
            return false;
        }
        ClassText = Working.Left(OpenParen).TrimEnd();
        ArgsText = Working.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
    }

    OutInst.Opcode = EAGIROpcode::Call;
    OutInst.ResultName = ResultName;
    FString ClassNameError;
    if (!FIrTextUtils::TryUnwrapNameToken(ClassText, OutInst.SymbolName, ClassNameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid call class name token: %s"), *ClassNameError),
            TEXT("AGIR_BAD_OPCODE"));
        return false;
    }
    if (!ParseArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
    {
        return false;
    }
    return true;
}

// Single-line opcode parse for any opcode that does NOT open a block. The
// surface form is `<opcode> [<name-token|class-token>] [name=value, ...]`. Used by
// transition / conduit / linked_anim / linked_input_pose / save_cached_pose /
// use_cached_pose / output / unassigned-call.
bool ParseSingleLineOpcode(EAGIROpcode Opcode, const FString& Rest, int32 LineNum,
    FAGIRInstruction& OutInst, TArray<FAGIRParseError>& OutErrors)
{
    OutInst.Opcode = Opcode;

    FString Working = Rest.TrimStartAndEnd();

    // Special handling for `transition "From" -> "To" ...`. The arrow pulls
    // the from/to names into Args alongside any priority=N rule=... etc.
    if (Opcode == EAGIROpcode::Transition)
    {
        const int32 ArrowIndex = Working.Find(TEXT("->"));
        if (ArrowIndex == INDEX_NONE)
        {
            AddError(OutErrors, LineNum, TEXT("transition requires 'From -> To'"),
                TEXT("AGIR_BAD_OPCODE"));
            return false;
        }

        FString FromText = Working.Left(ArrowIndex).TrimStartAndEnd();
        FString AfterArrow = Working.Mid(ArrowIndex + 2).TrimStart();

        FString FromName;
        FString ToName;
        FString NameError;
        if (!FIrTextUtils::TryUnwrapNameToken(FromText, FromName, NameError))
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Invalid transition source name: %s"), *NameError),
                TEXT("AGIR_BAD_OPCODE"));
            return false;
        }
        if (!ReadLeadingNameToken(AfterArrow, ToName, NameError))
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Invalid transition target name: %s"), *NameError),
                TEXT("AGIR_BAD_OPCODE"));
            return false;
        }

        FAGIRArg FromArg; FromArg.Name = TEXT("from"); FromArg.Value = FromName;
        FAGIRArg ToArg;   ToArg.Name = TEXT("to");     ToArg.Value = ToName;
        OutInst.Args.Add(MoveTemp(FromArg));
        OutInst.Args.Add(MoveTemp(ToArg));

        // Remaining text: space-separated `key=value` pairs (priority=N
        // rule=Foo bidirectional disabled etc.). Split on whitespace at top
        // level; each token is then a key=value (or bare flag).
        for (const FString& Token : SmartSplit(AfterArrow, TEXT(' ')))
        {
            const FString Trimmed = Token.TrimStartAndEnd();
            if (Trimmed.IsEmpty())
            {
                continue;
            }
            FAGIRArg Arg;
            const int32 EqualsIdx = Trimmed.Find(TEXT("="));
            if (EqualsIdx != INDEX_NONE)
            {
                Arg.Name = Trimmed.Left(EqualsIdx).TrimEnd();
                Arg.Value = Trimmed.Mid(EqualsIdx + 1).TrimStart();
            }
            else
            {
                Arg.Name = Trimmed;
                Arg.Value = TEXT("true");
            }
            OutInst.Args.Add(MoveTemp(Arg));
        }
        return true;
    }

    // For Output, the value is a free-form pose reference token. Stored in
    // SymbolName only — the compiler reads from there. Duplicating into Args
    // (`source=<ref>`) was dead weight; both fields held the same value.
    if (Opcode == EAGIROpcode::Output)
    {
        OutInst.SymbolName = Working;
        return true;
    }

    // For Conduit and Phase 3 cliff opcodes that lead with a name token
    // (blend_space, layered_blend, linked_input_pose): pull it into SymbolName
    // so the generic name=value scan below doesn't ingest the name token as a
    // flag arg.
    const bool bLeadsWithName =
        Opcode == EAGIROpcode::Conduit
        || Opcode == EAGIROpcode::StateAlias
        || Opcode == EAGIROpcode::BlendSpace
        || Opcode == EAGIROpcode::LayeredBlend
        || Opcode == EAGIROpcode::LinkedInputPose;
    if (bLeadsWithName)
    {
        if (!Working.IsEmpty())
        {
            FString NameError;
            if (!ReadLeadingNameToken(Working, OutInst.SymbolName, NameError))
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Invalid name token in '%s': %s"),
                        *FAGIRGrammar::OpcodeToText(Opcode), *NameError),
                    TEXT("AGIR_BAD_OPCODE"));
                return false;
            }
        }
    }

    // Generic remaining-text parse: space-separated `key=value` pairs.
    for (const FString& Token : SmartSplit(Working, TEXT(' ')))
    {
        const FString Trimmed = Token.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            continue;
        }
        FAGIRArg Arg;
        const int32 EqualsIdx = Trimmed.Find(TEXT("="));
        if (EqualsIdx != INDEX_NONE)
        {
            Arg.Name = Trimmed.Left(EqualsIdx).TrimEnd();
            Arg.Value = Trimmed.Mid(EqualsIdx + 1).TrimStart();
        }
        else
        {
            Arg.Name = Trimmed;
            Arg.Value = TEXT("true");
        }
        OutInst.Args.Add(MoveTemp(Arg));
    }
    return true;
}

// Parse a block-opening header line of the form
// `<opcode> "Name" [args ...] {`. The trailing `{` has already been consumed
// by the caller.
bool ParseBlockHeader(EAGIROpcode Opcode, const FString& Rest, int32 LineNum,
    FAGIRInstruction& OutInst, TArray<FAGIRParseError>& OutErrors)
{
    OutInst.Opcode = Opcode;
    FString Working = Rest.TrimStartAndEnd();

    FString NameError;
    if (!ReadLeadingNameToken(Working, OutInst.SymbolName, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid block header name token: %s"), *NameError),
            TEXT("AGIR_BAD_BLOCK_HEADER"));
        return false;
    }

    // Remaining text: space-separated `key=value` pairs.
    for (const FString& Token : SmartSplit(Working, TEXT(' ')))
    {
        const FString Trimmed = Token.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            continue;
        }
        FAGIRArg Arg;
        const int32 EqualsIdx = Trimmed.Find(TEXT("="));
        if (EqualsIdx != INDEX_NONE)
        {
            Arg.Name = Trimmed.Left(EqualsIdx).TrimEnd();
            Arg.Value = Trimmed.Mid(EqualsIdx + 1).TrimStart();
        }
        else
        {
            Arg.Name = Trimmed;
            Arg.Value = TEXT("true");
        }
        OutInst.Args.Add(MoveTemp(Arg));
    }
    return true;
}

// True for opcodes that introduce a `{ ... }` nested block.
bool IsBlockOpenerOpcode(EAGIROpcode Opcode)
{
    return Opcode == EAGIROpcode::StateMachine
        || Opcode == EAGIROpcode::State
        || Opcode == EAGIROpcode::BlendSpace
        || Opcode == EAGIROpcode::LayeredBlend
        || Opcode == EAGIROpcode::BlendSpaceSampleGraph
        || Opcode == EAGIROpcode::CustomTransitionBody;
}

// Transition is block-OPTIONAL: single-line form `transition From -> To attrs`
// or block form `transition From -> To attrs { custom_transition_body { ... } }`.
// Wave 3 introduces the block form to carry custom transition graph bodies.
bool IsBlockOptionalOpcode(EAGIROpcode Opcode)
{
    return Opcode == EAGIROpcode::Transition;
}

// One frame of the brace-stack. The frame's `Children` is the destination
// list for any instruction parsed at this nesting level. Top-level frames
// store a pointer to FAGIREntryBlock::Instructions; nested frames store a
// pointer to their owning instruction's Children.
struct FParseFrame
{
    TArray<TSharedPtr<FAGIRInstruction>>* Children = nullptr;
    int32 OpenLine = -1;
    bool bIsEntry = false;
};
}  // namespace

bool FAGIRParser::Parse(
    FStringView Code,
    TArray<FAGIREntryBlock>& OutBlocks,
    TArray<FAGIRParseError>& OutErrors,
    bool bSkipReferenceValidation)
{
    (void)bSkipReferenceValidation;  // Reserved for future cross-reference checks.

    OutBlocks.Reset();
    OutErrors.Reset();

    TArray<FString> Lines;
    FString CodeStr(Code.Len(), Code.GetData());
    CodeStr.ParseIntoArray(Lines, TEXT("\n"), false);

    TArray<FParseFrame> Stack;

    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const int32 LineNum = Index + 1;
        FString Line = StripTrailingComment(Lines[Index]).TrimStartAndEnd();
        if (Line.IsEmpty())
        {
            continue;
        }

        // Pure close-brace line pops one frame. A bare `}` at the top of the
        // stack with the entry frame still on it ends the entry block.
        if (Line == TEXT("}"))
        {
            if (Stack.Num() == 0)
            {
                AddError(OutErrors, LineNum, TEXT("Unbalanced '}': no open block"),
                    TEXT("AGIR_UNCLOSED_BLOCK"));
                return false;
            }
            Stack.Pop();
            continue;
        }

        // Inspect first token to decide routing.
        FIrToken FirstToken;
        if (!TryReadFirstToken(Line, FirstToken))
        {
            continue;
        }

        // Top-level interface manifest header: `interfaces {`. Sibling of the
        // `entry` block; carries `implements <classpath>` instructions only.
        const bool bIsInterfacesKeyword =
            FirstToken.Type == EIrTokenType::Keyword &&
            FirstToken.Text.ToLower() == TEXT("interfaces");

        if (bIsInterfacesKeyword)
        {
            if (Stack.Num() != 0)
            {
                AddError(OutErrors, LineNum,
                    TEXT("Nested 'interfaces' is not allowed; close the previous block first"),
                    TEXT("AGIR_BAD_BLOCK_HEADER"));
                continue;
            }

            FString Header = Line;
            const bool bHasOpenBrace = ConsumeTrailingOpenBrace(Header);
            if (!bHasOpenBrace)
            {
                AddError(OutErrors, LineNum,
                    TEXT("AGIR 'interfaces' header must end with '{'"),
                    TEXT("AGIR_BAD_BLOCK_HEADER"));
                continue;
            }

            // 'interfaces' takes no name token — header is just the keyword + '{'.
            FAGIREntryBlock NewBlock;
            NewBlock.Kind = EAGIREntryKind::InterfaceManifest;

            OutBlocks.Add(MoveTemp(NewBlock));
            FParseFrame Frame;
            Frame.Children = &OutBlocks.Last().Instructions;
            Frame.OpenLine = LineNum;
            Frame.bIsEntry = true;
            Stack.Add(Frame);
            continue;
        }

        // Top-level entry headers: `entry anim_graph <name-token> {`.
        const bool bIsEntryKeyword =
            FirstToken.Type == EIrTokenType::Keyword &&
            FirstToken.Text.ToLower() == TEXT("entry");

        if (bIsEntryKeyword)
        {
            if (Stack.Num() != 0)
            {
                AddError(OutErrors, LineNum,
                    TEXT("Nested 'entry' is not allowed; close the previous block first"),
                    TEXT("AGIR_BAD_BLOCK_HEADER"));
                continue;
            }

            FString Header = Line;
            const bool bHasOpenBrace = ConsumeTrailingOpenBrace(Header);
            if (!bHasOpenBrace)
            {
                AddError(OutErrors, LineNum,
                    TEXT("AGIR entry header must end with '{'"),
                    TEXT("AGIR_BAD_BLOCK_HEADER"));
                continue;
            }

            FAGIREntryBlock NewBlock;
            if (!ParseEntryHeader(Header, LineNum, NewBlock, OutErrors))
            {
                continue;
            }

            OutBlocks.Add(MoveTemp(NewBlock));
            FParseFrame Frame;
            Frame.Children = &OutBlocks.Last().Instructions;
            Frame.OpenLine = LineNum;
            Frame.bIsEntry = true;
            Stack.Add(Frame);
            continue;
        }

        // Any non-entry line must be inside a frame.
        if (Stack.Num() == 0)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Instruction outside of entry block: %s"), *Line),
                TEXT("AGIR_BAD_OPCODE"));
            continue;
        }

        // Strip trailing annotations (position + guid). They may live on any
        // node line, including block-opener lines.
        TSharedPtr<FAGIRInstruction> Instruction = MakeShared<FAGIRInstruction>();
        Instruction->SourceLine = LineNum;

        if (!StripAnnotations(Line, LineNum, *Instruction, OutErrors))
        {
            continue;
        }

        const bool bOpensBlock = ConsumeTrailingOpenBrace(Line);

        // Re-tokenize with annotations stripped to get the routing token.
        if (!TryReadFirstToken(Line, FirstToken))
        {
            continue;
        }

        // `%name = call ...` — assigned generic call.
        if (FirstToken.Type == EIrTokenType::PercentRef)
        {
            // Find the `=` after the percent ref.
            const FString After = Line.Mid(FirstToken.Position + FirstToken.Text.Len()).TrimStart();
            if (!After.StartsWith(TEXT("=")))
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Expected '=' after %s"), *FirstToken.Text),
                    TEXT("AGIR_BAD_OPCODE"));
                continue;
            }

            const FString ResultName = FirstToken.Text.Mid(1);  // strip leading `%`
            const FString Rest = After.Mid(1).TrimStart();

            if (!ParseAssignedCall(ResultName, Rest, LineNum, *Instruction, OutErrors))
            {
                continue;
            }

            if (bOpensBlock)
            {
                AddError(OutErrors, LineNum,
                    TEXT("Generic 'call' instruction does not open a block"),
                    TEXT("AGIR_BAD_OPCODE"));
                // Best-effort: still record the instruction; do not push a frame.
            }

            Stack.Last().Children->Add(Instruction);
            continue;
        }

        // Otherwise must be an opcode keyword.
        if (FirstToken.Type != EIrTokenType::Keyword)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Unknown AGIR instruction: %s"), *Line),
                TEXT("AGIR_BAD_OPCODE"));
            continue;
        }

        int32 OpcodeValue = 0;
        if (!FAGIRGrammar::Get().TryGetOpcode(FirstToken.Text, OpcodeValue))
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Unknown AGIR opcode: %s"), *FirstToken.Text),
                TEXT("AGIR_BAD_OPCODE"));
            continue;
        }

        const EAGIROpcode Opcode = static_cast<EAGIROpcode>(OpcodeValue);
        const FString Rest = Line.Mid(FirstToken.Position + FirstToken.Text.Len()).TrimStart();

        if (bOpensBlock)
        {
            if (!IsBlockOpenerOpcode(Opcode) && !IsBlockOptionalOpcode(Opcode))
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Opcode '%s' does not open a '{' block"), *FirstToken.Text),
                    TEXT("AGIR_BAD_BLOCK_HEADER"));
                continue;
            }

            // Block-optional opcodes (currently only Transition) reuse the
            // single-line parse for the head (`From -> To attrs`) and then push
            // a child frame for the body. CustomTransitionBody itself has no
            // head payload — its name is just the keyword.
            if (IsBlockOptionalOpcode(Opcode))
            {
                if (!ParseSingleLineOpcode(Opcode, Rest, LineNum, *Instruction, OutErrors))
                {
                    continue;
                }
            }
            else if (Opcode == EAGIROpcode::CustomTransitionBody)
            {
                // No head payload; the keyword + `{` is enough.
                Instruction->Opcode = Opcode;
            }
            else
            {
                if (!ParseBlockHeader(Opcode, Rest, LineNum, *Instruction, OutErrors))
                {
                    continue;
                }
            }

            Stack.Last().Children->Add(Instruction);

            FParseFrame Frame;
            Frame.Children = &Instruction->Children;
            Frame.OpenLine = LineNum;
            Frame.bIsEntry = false;
            Stack.Add(Frame);
            continue;
        }

        // Single-line opcode form.
        if (IsBlockOpenerOpcode(Opcode))
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Opcode '%s' requires a '{' block"), *FirstToken.Text),
                TEXT("AGIR_BAD_BLOCK_HEADER"));
            continue;
        }

        if (Opcode == EAGIROpcode::Call)
        {
            AddError(OutErrors, LineNum,
                TEXT("'call' requires a result assignment: '%name = call <ClassPath>(...)'"),
                TEXT("AGIR_BAD_OPCODE"));
            continue;
        }

        // `implements` is only valid directly under an `interfaces { ... }`
        // header — InterfaceManifest blocks have no nested children, so the
        // owning entry block is always OutBlocks.Last().
        if (Opcode == EAGIROpcode::Implements)
        {
            const bool bIsInsideInterfaces =
                OutBlocks.Num() > 0 &&
                OutBlocks.Last().Kind == EAGIREntryKind::InterfaceManifest;
            if (!bIsInsideInterfaces)
            {
                AddError(OutErrors, LineNum,
                    TEXT("'implements' is only valid inside an 'interfaces { ... }' block"),
                    TEXT("AGIR_BAD_OPCODE"));
                continue;
            }

            FString ClassPathRest = Rest;
            FString ClassPath;
            FString NameError;
            if (!ReadLeadingNameToken(ClassPathRest, ClassPath, NameError))
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Invalid 'implements' classpath: %s"), *NameError),
                    TEXT("AGIR_BAD_OPCODE"));
                continue;
            }
            Instruction->Opcode = EAGIROpcode::Implements;
            Instruction->SymbolName = ClassPath;
            Stack.Last().Children->Add(Instruction);
            continue;
        }

        if (!ParseSingleLineOpcode(Opcode, Rest, LineNum, *Instruction, OutErrors))
        {
            continue;
        }

        Stack.Last().Children->Add(Instruction);
    }

    if (Stack.Num() != 0)
    {
        AddError(OutErrors, Lines.Num(),
            TEXT("Unclosed AGIR block at end of input"),
            TEXT("AGIR_UNCLOSED_BLOCK"));
        return false;
    }

    return OutErrors.Num() == 0;
}
