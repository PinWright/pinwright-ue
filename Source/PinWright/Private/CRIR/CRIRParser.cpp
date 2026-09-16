// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRParser.h"

#include "CRIR/CRIRGrammar.h"
#include "IrCore/IrTextUtils.h"
#include "IrCore/IrTokenizer.h"

namespace
{
void AddError(TArray<FCRIRParseError>& Errors, int32 Line, const FString& Message, const FString& Code = FString())
{
    Errors.Add(FCRIRParseError(Line, Message, Code));
}

FString StripTrailingComment(const FString& Value)
{
    return FIrTextUtils::StripTrailingComment(Value);
}

int32 FindMatchingChar(const FString& Str, int32 OpenPos, TCHAR OpenChar, TCHAR CloseChar)
{
    return FIrTextUtils::FindMatchingChar(Str, OpenPos, OpenChar, CloseChar);
}

TArray<int32> FindTopLevelDelimiterPositions(const FString& Str, TCHAR Delimiter, bool bStopAtFirst = false)
{
    return FIrTextUtils::FindTopLevelDelimiterPositions(Str, Delimiter, bStopAtFirst);
}

TArray<FString> SmartSplit(const FString& Str, TCHAR Delimiter)
{
    return FIrTextUtils::SmartSplit(Str, Delimiter);
}

bool TryReadFirstToken(const FString& Line, FIrToken& OutToken)
{
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Line, FCRIRGrammar::Get());
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
        // Bare identifiers stop at whitespace OR at '(' — the emitter writes
        // `invoke_entry SubRoutine(args)` with no space before the arg-list
        // open paren, so without this the parser would swallow the parens and
        // everything inside as part of the name.
        int32 EndIdx = 0;
        while (EndIdx < Working.Len()
            && !FChar::IsWhitespace(Working[EndIdx])
            && Working[EndIdx] != TEXT('('))
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

bool TryExtractPosition(FString& InOutLine, FCRIRPosition& OutPosition)
{
    FVector2D Pos = FVector2D::ZeroVector;
    FString Error;
    const bool bFound = FIrTextUtils::TryExtractPosition(InOutLine, Pos, Error);
    if (bFound && Error.IsEmpty())
    {
        OutPosition.X = static_cast<float>(Pos.X);
        OutPosition.Y = static_cast<float>(Pos.Y);
        OutPosition.bSet = true;
        return true;
    }
    return false;
}

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

// Parse one `name=value` or bare-flag token from a single space-separated arg
// list segment. Wire references on the RHS (`name=%node.pin`) decompose into
// the FCRIRArg ref fields; everything else is preserved as RawText.
void ParseArgToken(const FString& Trimmed, FCRIRArg& OutArg)
{
    const int32 EqualsIdx = Trimmed.Find(TEXT("="));
    if (EqualsIdx == INDEX_NONE)
    {
        OutArg.Name = Trimmed;
        OutArg.RawText = TEXT("true");
        return;
    }

    OutArg.Name = Trimmed.Left(EqualsIdx).TrimEnd();
    FString Value = Trimmed.Mid(EqualsIdx + 1).TrimStart();

    if (Value.StartsWith(TEXT("%")))
    {
        // Wire ref: %nodeName.pinName (pin part may itself contain dots when
        // addressing struct sub-pins like Transform.Translation, so split on the
        // first dot only).
        const FString WithoutPercent = Value.Mid(1);
        const int32 DotIdx = WithoutPercent.Find(TEXT("."));
        if (DotIdx != INDEX_NONE)
        {
            OutArg.bIsLocalRef = true;
            OutArg.LocalRefNode = WithoutPercent.Left(DotIdx);
            OutArg.LocalRefPin = WithoutPercent.Mid(DotIdx + 1);
            OutArg.RawText = Value;
            return;
        }
        // Bare %node with no pin component — treat as ref with empty pin so
        // the compiler can decide how to route (e.g. node-level link to the
        // unit's sole output).
        OutArg.bIsLocalRef = true;
        OutArg.LocalRefNode = WithoutPercent;
        OutArg.RawText = Value;
        return;
    }

    OutArg.RawText = Value;
}

// Top-level split-on-comma across paren-balanced text. Mirrors AGIR's
// ParseArgList shape but uses `=` separators (CRIR convention) rather than
// `:`, since CRIR matches BPIR/MGIR's `name=value` arg form.
bool ParseUnitArgList(const FString& ArgsText, int32 LineNum, TArray<FCRIRParseError>& OutErrors, TArray<FCRIRArg>& OutArgs)
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
        FCRIRArg Arg;
        ParseArgToken(Pair, Arg);
        if (Arg.Name.IsEmpty())
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Invalid CRIR arg with empty name: '%s'"), *Pair),
                TEXT("CRIR_BAD_ARG"));
            return false;
        }
        OutArgs.Add(MoveTemp(Arg));
    }
    return true;
}

// Shared prologue for `%localId = <opcode> ...` parsers. Verifies ResultName,
// stamps OutInst.Opcode/LocalId, tokenizes RestAfterEquals, checks the first
// token matches ExpectedKeyword, and emits OutWorking advanced past the
// keyword. Error messages and codes match the per-parser originals exactly so
// downstream tests keying on text still hit.
bool ConsumeOpcodeKeyword(
    ECRIROpcode ExpectedOpcode,
    const TCHAR* ExpectedKeyword,
    const FString& ResultName,
    const FString& RestAfterEquals,
    int32 LineNum,
    FCRIRInstruction& OutInst,
    FString& OutWorking,
    TArray<FCRIRParseError>& OutErrors)
{
    if (ResultName.IsEmpty())
    {
        AddError(OutErrors, LineNum, TEXT("CRIR result name is empty"), TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    OutInst.Opcode = ExpectedOpcode;
    OutInst.LocalId = ResultName;

    OutWorking = RestAfterEquals.TrimStartAndEnd();
    const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(OutWorking, FCRIRGrammar::Get());
    if (Tokens.Num() == 0 || Tokens[0].Type != EIrTokenType::Keyword || Tokens[0].Text.ToLower() != ExpectedKeyword)
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Expected '%s' opcode after '%%%s ='"), ExpectedKeyword, *ResultName),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    OutWorking = OutWorking.Mid(Tokens[0].Position + Tokens[0].Text.Len()).TrimStart();
    return true;
}

// Parse `%localId = unit /Script/Path.Foo(arg=val, ...) [@(x,y)]`. Position
// has already been stripped from RestAfterEquals. Returns false on failure.
bool ParseUnitInstruction(const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(ECRIROpcode::Unit, TEXT("unit"), ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }

    const int32 OpenParen = Working.Find(TEXT("("));
    if (OpenParen == INDEX_NONE)
    {
        OutInst.StructPath = Working.TrimEnd();
        if (OutInst.StructPath.IsEmpty())
        {
            AddError(OutErrors, LineNum,
                TEXT("'unit' requires a struct path"),
                TEXT("CRIR_BAD_OPCODE"));
            return false;
        }
        return true;
    }

    const int32 CloseParen = FindMatchingChar(Working, OpenParen, TEXT('('), TEXT(')'));
    if (CloseParen == INDEX_NONE)
    {
        AddError(OutErrors, LineNum,
            TEXT("Unmatched '(' in 'unit' argument list"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    OutInst.StructPath = Working.Left(OpenParen).TrimEnd();
    if (OutInst.StructPath.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            TEXT("'unit' struct path is empty"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    const FString ArgsText = Working.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
    if (!ParseUnitArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
    {
        return false;
    }

    return true;
}

// Parse `%localId = var Name Type [= literal] [@(x,y)]`. Position already
// stripped from RestAfterEquals. The literal RHS (after the second `=`) is
// captured verbatim; the type spec is captured raw and parsed by the compiler
// via FIrTypeSpecParser.
bool ParseVarInstruction(const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(ECRIROpcode::Var, TEXT("var"), ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }

    // Split on first top-level `=` to separate `Name Type` from optional default literal.
    const TArray<int32> EqualsPositions = FindTopLevelDelimiterPositions(Working, TEXT('='), true);
    FString HeadText;
    if (EqualsPositions.Num() > 0)
    {
        const int32 EqIdx = EqualsPositions[0];
        HeadText = Working.Left(EqIdx).TrimEnd();
        OutInst.VarDefault = Working.Mid(EqIdx + 1).TrimStart();
    }
    else
    {
        HeadText = Working;
    }

    // Pull leading name token, then the rest is the type spec.
    FString NameError;
    if (!ReadLeadingNameToken(HeadText, OutInst.VarName, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid 'var' name: %s"), *NameError),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    OutInst.VarType = HeadText.TrimStartAndEnd();
    if (OutInst.VarType.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("'var %s' missing type spec"), *OutInst.VarName),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    return true;
}

// Parse `%localId = reroute <Type> [= <literal>] [(args)]`. Position already
// stripped. Type goes into VarType, optional literal into VarDefault, optional
// wire/value args into Args.
bool ParseRerouteInstruction(const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(ECRIROpcode::Reroute, TEXT("reroute"), ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }

    // Split off trailing `(args)` if present.
    FString ArgsText;
    const int32 OpenParen = Working.Find(TEXT("("));
    if (OpenParen != INDEX_NONE)
    {
        const int32 CloseParen = FindMatchingChar(Working, OpenParen, TEXT('('), TEXT(')'));
        if (CloseParen == INDEX_NONE)
        {
            AddError(OutErrors, LineNum,
                TEXT("Unmatched '(' in 'reroute' arg list"),
                TEXT("CRIR_BAD_OPCODE"));
            return false;
        }
        ArgsText = Working.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
        Working = (Working.Left(OpenParen) + Working.Mid(CloseParen + 1)).TrimStartAndEnd();
    }

    // Optional `= default` on the remaining head text.
    const TArray<int32> EqualsPositions = FindTopLevelDelimiterPositions(Working, TEXT('='), true);
    if (EqualsPositions.Num() > 0)
    {
        const int32 EqIdx = EqualsPositions[0];
        OutInst.VarType = Working.Left(EqIdx).TrimEnd();
        OutInst.VarDefault = Working.Mid(EqIdx + 1).TrimStart();
    }
    else
    {
        OutInst.VarType = Working.TrimEnd();
    }

    if (OutInst.VarType.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            TEXT("'reroute' requires a type spec"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    if (!ArgsText.IsEmpty())
    {
        if (!ParseUnitArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
        {
            return false;
        }
    }
    return true;
}

// Parse `comment "<text>" [size=(w,h)] [color=(r,g,b,a)]`. No `%localId =`
// prefix — comments have no incoming wires, so they don't participate in the
// SSA-style reference graph. Stores text in VarName, size in VarType, color
// in VarDefault — all already-tuple-formatted strings.
bool ParseCommentInstruction(const FString& Rest, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    OutInst.Opcode = ECRIROpcode::Comment;
    OutInst.SourceLine = LineNum;

    FString Working = Rest.TrimStartAndEnd();
    // Leading quoted text. Use the same ReadLeadingNameToken which handles
    // quoted strings — unwraps to plain text.
    FString TextError;
    if (!ReadLeadingNameToken(Working, OutInst.VarName, TextError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid 'comment' text: %s"), *TextError),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    // Walk remaining `key=value` attributes the same way element parsing does.
    int32 Cursor = 0;
    while (Cursor < Working.Len())
    {
        while (Cursor < Working.Len() && FChar::IsWhitespace(Working[Cursor]))
        {
            ++Cursor;
        }
        if (Cursor >= Working.Len())
        {
            break;
        }
        const int32 KeyStart = Cursor;
        while (Cursor < Working.Len() && Working[Cursor] != TEXT('=') && !FChar::IsWhitespace(Working[Cursor]))
        {
            ++Cursor;
        }
        const FString Key = Working.Mid(KeyStart, Cursor - KeyStart);
        if (Cursor >= Working.Len() || Working[Cursor] != TEXT('='))
        {
            // Bare flag — ignore for comments.
            continue;
        }
        ++Cursor; // skip '='
        if (Cursor >= Working.Len())
        {
            break;
        }

        const TCHAR First = Working[Cursor];
        FString Value;
        if (First == TEXT('('))
        {
            const int32 End = FindMatchingChar(Working, Cursor, TEXT('('), TEXT(')'));
            if (End == INDEX_NONE)
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Unmatched '(' in 'comment' attribute '%s'"), *Key),
                    TEXT("CRIR_BAD_OPCODE"));
                return false;
            }
            Value = Working.Mid(Cursor, End - Cursor + 1);
            Cursor = End + 1;
        }
        else
        {
            const int32 ValueStart = Cursor;
            while (Cursor < Working.Len() && !FChar::IsWhitespace(Working[Cursor]))
            {
                ++Cursor;
            }
            Value = Working.Mid(ValueStart, Cursor - ValueStart);
        }

        const FString KeyLower = Key.ToLower();
        if (KeyLower == TEXT("size"))
        {
            OutInst.VarType = Value;
        }
        else if (KeyLower == TEXT("color"))
        {
            OutInst.VarDefault = Value;
        }
        // Unknown comment attrs are ignored; Phase A doesn't expose them.
    }
    return true;
}

// Parse `%localId = <opcode> <CPPType> [(args)]`. Used by if/select — both
// take the result type as a head expression and optionally trailing args.
bool ParseIfOrSelectInstruction(ECRIROpcode Opcode, const FString& ResultName,
    const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    const TCHAR* OpKeyword = (Opcode == ECRIROpcode::If) ? TEXT("if") : TEXT("select");

    FString Working;
    if (!ConsumeOpcodeKeyword(Opcode, OpKeyword, ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }

    FString ArgsText;
    const int32 OpenParen = Working.Find(TEXT("("));
    if (OpenParen != INDEX_NONE)
    {
        const int32 CloseParen = FindMatchingChar(Working, OpenParen, TEXT('('), TEXT(')'));
        if (CloseParen == INDEX_NONE)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Unmatched '(' in '%s' arg list"), OpKeyword),
                TEXT("CRIR_BAD_OPCODE"));
            return false;
        }
        ArgsText = Working.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
        Working = (Working.Left(OpenParen) + Working.Mid(CloseParen + 1)).TrimStartAndEnd();
    }

    OutInst.StructPath = Working.TrimEnd();
    if (OutInst.StructPath.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("'%s' requires a CPP type spec"), OpKeyword),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    if (!ArgsText.IsEmpty())
    {
        if (!ParseUnitArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
        {
            return false;
        }
    }
    return true;
}

// Parse `%localId = enum <EnumPath> [= <Value>]`. Path goes into StructPath,
// optional value into VarDefault.
bool ParseEnumInstruction(const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(ECRIROpcode::Enum, TEXT("enum"), ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }
    const TArray<int32> EqualsPositions = FindTopLevelDelimiterPositions(Working, TEXT('='), true);
    if (EqualsPositions.Num() > 0)
    {
        const int32 EqIdx = EqualsPositions[0];
        OutInst.StructPath = Working.Left(EqIdx).TrimEnd();
        OutInst.VarDefault = Working.Mid(EqIdx + 1).TrimStart();
    }
    else
    {
        OutInst.StructPath = Working.TrimEnd();
    }
    if (OutInst.StructPath.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            TEXT("'enum' requires an enum object path"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    return true;
}

// Parse `%localId = invoke_entry <EntryName> [(args)]`. Name goes into VarName.
bool ParseInvokeEntryInstruction(const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(ECRIROpcode::InvokeEntry, TEXT("invoke_entry"), ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }
    FString NameError;
    if (!ReadLeadingNameToken(Working, OutInst.VarName, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid 'invoke_entry' name: %s"), *NameError),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    // Optional trailing `(args)`.
    Working = Working.TrimStart();
    if (Working.StartsWith(TEXT("(")))
    {
        const int32 CloseParen = FindMatchingChar(Working, 0, TEXT('('), TEXT(')'));
        if (CloseParen == INDEX_NONE)
        {
            AddError(OutErrors, LineNum,
                TEXT("Unmatched '(' in 'invoke_entry' arg list"),
                TEXT("CRIR_BAD_OPCODE"));
            return false;
        }
        const FString ArgsText = Working.Mid(1, CloseParen - 1);
        if (!ParseUnitArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
        {
            return false;
        }
    }
    return true;
}

// Parse `%localId = <opcode> <NameToken> [(args)]`. Shared by template and
// dispatch — both carry a single leading name-or-notation token (notation may
// contain `(`, `,`, `:`, so it must be backtick-quoted) followed by an optional
// `(args)` clause. The leading token lands in StructPath.
bool ParseTemplateOrDispatchInstruction(ECRIROpcode Opcode, const TCHAR* OpKeyword,
    const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(Opcode, OpKeyword, ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }

    FString NameError;
    if (!ReadLeadingNameToken(Working, OutInst.StructPath, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid '%s' token: %s"), OpKeyword, *NameError),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    if (OutInst.StructPath.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("'%s' requires a %s token"),
                OpKeyword,
                Opcode == ECRIROpcode::Template ? TEXT("notation") : TEXT("factory struct name")),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    Working = Working.TrimStart();
    if (Working.StartsWith(TEXT("(")))
    {
        const int32 CloseParen = FindMatchingChar(Working, 0, TEXT('('), TEXT(')'));
        if (CloseParen == INDEX_NONE)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Unmatched '(' in '%s' arg list"), OpKeyword),
                TEXT("CRIR_BAD_OPCODE"));
            return false;
        }
        const FString ArgsText = Working.Mid(1, CloseParen - 1);
        if (!ParseUnitArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
        {
            return false;
        }
    }
    return true;
}

// Parse the collapse instruction head: `%localId = collapse "<NodeName>"`.
// The trailing `{` is consumed by the outer loop (it has already been stripped
// from RestAfterEquals by the caller via ConsumeTrailingOpenBrace) and the
// body is collected by pushing a frame routed at OutInst.Children.
bool ParseCollapseInstruction(const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(ECRIROpcode::Collapse, TEXT("collapse"), ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }

    FString NameError;
    if (!ReadLeadingNameToken(Working, OutInst.VarName, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid 'collapse' node name: %s"), *NameError),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    if (OutInst.VarName.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            TEXT("'collapse' requires a node-name token"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    return true;
}

// Parse `%localId = function_ref <Token>` where Token is either `Name`
// (same-asset library) or `HostPath::Name` (external). The token lands in
// StructPath; the compiler splits on `::` to discriminate.
bool ParseFunctionRefInstruction(const FString& ResultName, const FString& RestAfterEquals, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    FString Working;
    if (!ConsumeOpcodeKeyword(ECRIROpcode::FunctionRef, TEXT("function_ref"), ResultName, RestAfterEquals, LineNum, OutInst, Working, OutErrors))
    {
        return false;
    }

    FString NameError;
    if (!ReadLeadingNameToken(Working, OutInst.StructPath, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid 'function_ref' token: %s"), *NameError),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    if (OutInst.StructPath.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            TEXT("'function_ref' requires a function-name token (or HostPath::Name)"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }

    Working = Working.TrimStart();
    if (Working.StartsWith(TEXT("(")))
    {
        const int32 CloseParen = FindMatchingChar(Working, 0, TEXT('('), TEXT(')'));
        if (CloseParen == INDEX_NONE)
        {
            AddError(OutErrors, LineNum,
                TEXT("Unmatched '(' in 'function_ref' arg list"),
                TEXT("CRIR_BAD_OPCODE"));
            return false;
        }
        const FString ArgsText = Working.Mid(1, CloseParen - 1);
        if (!ParseUnitArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
        {
            return false;
        }
    }
    return true;
}

// Parse `function_entry [(args)]` or `function_return [(args)]`. No LocalId.
bool ParseFunctionInterfaceInstruction(ECRIROpcode Opcode, const TCHAR* Keyword,
    const FString& Rest, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    OutInst.Opcode = Opcode;
    OutInst.SourceLine = LineNum;

    FString Working = Rest.TrimStartAndEnd();
    if (Working.StartsWith(TEXT("(")))
    {
        const int32 CloseParen = FindMatchingChar(Working, 0, TEXT('('), TEXT(')'));
        if (CloseParen == INDEX_NONE)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Unmatched '(' in '%s' arg list"), Keyword),
                TEXT("CRIR_BAD_OPCODE"));
            return false;
        }
        const FString ArgsText = Working.Mid(1, CloseParen - 1);
        if (!ParseUnitArgList(ArgsText, LineNum, OutErrors, OutInst.Args))
        {
            return false;
        }
        Working = Working.Mid(CloseParen + 1).TrimStartAndEnd();
    }
    if (!Working.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Unexpected trailing text on '%s': '%s'"), Keyword, *Working),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    return true;
}

// Parse `exposed_pin <Name>: <input|output|io> <CPPType> [object=<path>] [= <default>]`.
// Mirror of FCRIRTextEmitter::EmitExposedPin. The name and direction tokens
// are required; CPPType is required; the object= and = clauses are optional.
bool ParseExposedPinInstruction(const FString& Rest, int32 LineNum,
    FCRIRInstruction& OutInst, TArray<FCRIRParseError>& OutErrors)
{
    OutInst.Opcode = ECRIROpcode::ExposedPin;
    OutInst.SourceLine = LineNum;

    FString Working = Rest.TrimStartAndEnd();

    // Pin name: bare identifier up to the first ':'.
    const int32 ColonIndex = Working.Find(TEXT(":"));
    if (ColonIndex == INDEX_NONE)
    {
        AddError(OutErrors, LineNum,
            TEXT("'exposed_pin' requires '<Name>: <direction> <CPPType> ...'"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    OutInst.VarName = Working.Left(ColonIndex).TrimStartAndEnd();
    if (OutInst.VarName.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            TEXT("'exposed_pin' requires a pin-name token before ':'"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    Working = Working.Mid(ColonIndex + 1).TrimStart();

    // Direction token: input / output / io.
    int32 DirEnd = 0;
    while (DirEnd < Working.Len() && !FChar::IsWhitespace(Working[DirEnd]))
    {
        ++DirEnd;
    }
    const FString DirToken = Working.Left(DirEnd).ToLower();
    if (DirToken == TEXT("input"))      { OutInst.ExposedDirection = ECRIRExposedPinDirection::Input; }
    else if (DirToken == TEXT("output")) { OutInst.ExposedDirection = ECRIRExposedPinDirection::Output; }
    else if (DirToken == TEXT("io"))     { OutInst.ExposedDirection = ECRIRExposedPinDirection::IO; }
    else
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("'exposed_pin' direction must be 'input', 'output', or 'io'; got '%s'"), *DirToken),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    Working = Working.Mid(DirEnd).TrimStart();

    // CPPType: bare token up to whitespace or '='. Must be present.
    int32 TypeEnd = 0;
    while (TypeEnd < Working.Len() && !FChar::IsWhitespace(Working[TypeEnd]) && Working[TypeEnd] != TEXT('='))
    {
        ++TypeEnd;
    }
    OutInst.VarType = Working.Left(TypeEnd).TrimStartAndEnd();
    if (OutInst.VarType.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            TEXT("'exposed_pin' requires a CPPType token after direction"),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    Working = Working.Mid(TypeEnd).TrimStart();

    // Optional `object=<path>` clause.
    if (Working.StartsWith(TEXT("object=")))
    {
        Working = Working.Mid(7).TrimStart();
        int32 PathEnd = 0;
        while (PathEnd < Working.Len() && !FChar::IsWhitespace(Working[PathEnd]) && Working[PathEnd] != TEXT('='))
        {
            ++PathEnd;
        }
        OutInst.ExposedTypeObjectPath = Working.Left(PathEnd).TrimStartAndEnd();
        Working = Working.Mid(PathEnd).TrimStart();
    }

    // Optional `= <default>` — everything after the equals sign is the default
    // literal (trimmed). Default may contain spaces / parens / quotes.
    if (Working.StartsWith(TEXT("=")))
    {
        OutInst.VarDefault = Working.Mid(1).TrimStartAndEnd();
    }
    else if (!Working.IsEmpty())
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Unexpected trailing text on 'exposed_pin': '%s'"), *Working),
            TEXT("CRIR_BAD_OPCODE"));
        return false;
    }
    return true;
}

// Parse a `bone "Name" parent="X" location=(x,y,z) ...` element line. Element
// kind has already been classified by the caller. All key=value pairs go into
// Attributes; the only fields with first-class storage are Name and Parent
// since every element kind has those.
bool ParseElementInstruction(ECRIRElementKind Kind, const FString& Rest, int32 LineNum,
    FCRIRElementInstruction& OutElem, TArray<FCRIRParseError>& OutErrors)
{
    OutElem.Kind = Kind;
    OutElem.SourceLine = LineNum;

    FString Working = Rest.TrimStartAndEnd();
    FString NameError;
    if (!ReadLeadingNameToken(Working, OutElem.Name, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid element name: %s"), *NameError),
            TEXT("CRIR_BAD_ELEMENT"));
        return false;
    }

    // Walk the remaining text manually so values containing `=` (e.g. raw
    // serialized struct text) inside paren / bracket groups stay intact. The
    // grammar at this level is: zero or more `key=value` pairs separated by
    // whitespace, where value may be a quoted string, parenthesized tuple,
    // bracketed list, or bare token.
    int32 Cursor = 0;
    while (Cursor < Working.Len())
    {
        while (Cursor < Working.Len() && FChar::IsWhitespace(Working[Cursor]))
        {
            ++Cursor;
        }
        if (Cursor >= Working.Len())
        {
            break;
        }

        const int32 KeyStart = Cursor;
        while (Cursor < Working.Len() && Working[Cursor] != TEXT('=') && !FChar::IsWhitespace(Working[Cursor]))
        {
            ++Cursor;
        }
        const FString Key = Working.Mid(KeyStart, Cursor - KeyStart);

        if (Cursor >= Working.Len() || Working[Cursor] != TEXT('='))
        {
            // Bare flag token without `=` — store as "true" so callers can
            // detect presence with a uniform map lookup.
            if (!Key.IsEmpty())
            {
                OutElem.Attributes.Add(Key, TEXT("true"));
            }
            continue;
        }
        ++Cursor;  // skip `=`

        if (Cursor >= Working.Len())
        {
            OutElem.Attributes.Add(Key, FString());
            break;
        }

        const TCHAR First = Working[Cursor];
        FString Value;
        if (First == TEXT('"'))
        {
            int32 End = INDEX_NONE;
            for (int32 Index = Cursor + 1; Index < Working.Len(); ++Index)
            {
                if (Working[Index] == TEXT('"') && FIrTextUtils::IsUnescapedQuote(Working, Index))
                {
                    End = Index;
                    break;
                }
            }
            if (End == INDEX_NONE)
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Unterminated string in element attribute '%s'"), *Key),
                    TEXT("CRIR_BAD_ELEMENT"));
                return false;
            }
            Value = Working.Mid(Cursor + 1, End - Cursor - 1);
            Cursor = End + 1;
        }
        else if (First == TEXT('(') || First == TEXT('[') || First == TEXT('{'))
        {
            const TCHAR Close = (First == TEXT('(')) ? TEXT(')') : (First == TEXT('[')) ? TEXT(']') : TEXT('}');
            const int32 End = FindMatchingChar(Working, Cursor, First, Close);
            if (End == INDEX_NONE)
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Unmatched '%c' in element attribute '%s'"), First, *Key),
                    TEXT("CRIR_BAD_ELEMENT"));
                return false;
            }
            Value = Working.Mid(Cursor, End - Cursor + 1);
            Cursor = End + 1;
        }
        else
        {
            // Bareword value — read until whitespace, but transparently skip
            // through any nested (...) / [...] / {...} / "..." groups so that
            // function-call-shaped literals (e.g. `position(1, 2, 3)`,
            // `transform(loc=(0,0,0), rot=(0,0,0), scale=(1,1,1))`) survive
            // intact with their internal commas and spaces.
            const int32 ValueStart = Cursor;
            while (Cursor < Working.Len() && !FChar::IsWhitespace(Working[Cursor]))
            {
                const TCHAR C = Working[Cursor];
                if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{'))
                {
                    const TCHAR Close = (C == TEXT('(')) ? TEXT(')')
                        : (C == TEXT('[')) ? TEXT(']') : TEXT('}');
                    const int32 GroupEnd = FindMatchingChar(Working, Cursor, C, Close);
                    if (GroupEnd == INDEX_NONE)
                    {
                        AddError(OutErrors, LineNum,
                            FString::Printf(TEXT("Unmatched '%c' in element attribute '%s'"), C, *Key),
                            TEXT("CRIR_BAD_ELEMENT"));
                        return false;
                    }
                    Cursor = GroupEnd + 1;
                    continue;
                }
                if (C == TEXT('"'))
                {
                    int32 EndQ = INDEX_NONE;
                    for (int32 Idx = Cursor + 1; Idx < Working.Len(); ++Idx)
                    {
                        if (Working[Idx] == TEXT('"') && FIrTextUtils::IsUnescapedQuote(Working, Idx))
                        {
                            EndQ = Idx;
                            break;
                        }
                    }
                    if (EndQ == INDEX_NONE)
                    {
                        AddError(OutErrors, LineNum,
                            FString::Printf(TEXT("Unterminated string in element attribute '%s'"), *Key),
                            TEXT("CRIR_BAD_ELEMENT"));
                        return false;
                    }
                    Cursor = EndQ + 1;
                    continue;
                }
                ++Cursor;
            }
            Value = Working.Mid(ValueStart, Cursor - ValueStart);
        }

        if (Key.ToLower() == TEXT("parent"))
        {
            OutElem.Parent = Value;
        }
        else
        {
            OutElem.Attributes.Add(Key, Value);
        }
    }

    return true;
}

// Validate one flat scope: every wire-ref must resolve to a `%localId`
// declared *somewhere* in this scope. Sub-graph bodies (collapse Children) are
// validated recursively with a *fresh* scope — wires do not cross sub-graph
// boundaries.
//
// Two-pass within a scope: Pass 1 collects every `%localId` declaration, Pass 2
// checks wires against that complete set. Forward references (a wire whose
// source `%nN` is declared on a *later* line of the same scope) are therefore
// accepted. This is required because the decompiler sorts a graph's nodes by
// UObject name (CRIRDecompiler.cpp EmitGraphBody), not by exec/topological
// order, so a wire source can legitimately be emitted after its sink — making
// `decompile → compile → decompile` byte-equal (crir-language-reference.md:489)
// only hold once forward refs validate. The compiler engine already tolerates
// this: it creates all nodes in Pass A and defers wiring to Pass B precisely
// because `%localId` references may point at later-declared nodes
// (CRIRCompiler.cpp CompileInstructionsIntoGraph). Aligning the upfront gate
// with that engine also lets hand-authored CRIR forward-reference freely. A
// single missing/typo'd id still errors because it is absent from both passes.
bool ValidateInstructionScope(const TArray<FCRIRInstruction>& Instructions, TArray<FCRIRParseError>& OutErrors)
{
    // Pass 1 — collect all local-id declarations in this scope.
    TSet<FString> KnownIds;
    for (const FCRIRInstruction& Inst : Instructions)
    {
        if (!Inst.LocalId.IsEmpty())
        {
            KnownIds.Add(Inst.LocalId);
        }
    }

    // Pass 2 — validate wire references (forward refs now resolve) and recurse
    // into sub-graph bodies with a fresh scope.
    bool bOk = true;
    for (const FCRIRInstruction& Inst : Instructions)
    {
        for (const FCRIRArg& Arg : Inst.Args)
        {
            if (!Arg.bIsLocalRef)
            {
                continue;
            }
            if (!KnownIds.Contains(Arg.LocalRefNode))
            {
                AddError(OutErrors, Inst.SourceLine,
                    FString::Printf(TEXT("Wire arg '%s=%%%s.%s' references undefined node '%%%s'"),
                        *Arg.Name, *Arg.LocalRefNode, *Arg.LocalRefPin, *Arg.LocalRefNode),
                    TEXT("CRIR_UNDEFINED_REF"));
                bOk = false;
            }
        }
        if (Inst.Opcode == ECRIROpcode::Collapse && Inst.Children.Num() > 0)
        {
            if (!ValidateInstructionScope(Inst.Children, OutErrors))
            {
                bOk = false;
            }
        }
    }
    return bOk;
}

bool ValidateBlockReferences(const FCRIREntryBlock& Block, TArray<FCRIRParseError>& OutErrors)
{
    if (Block.Kind != ECRIREntryKind::RigGraph && Block.Kind != ECRIREntryKind::RigFunction)
    {
        return true;
    }
    return ValidateInstructionScope(Block.Instructions, OutErrors);
}

// Parse the top-level header `rig_graph "<name>" {` or `rig_hierarchy {`. The
// trailing `{` has already been consumed.
bool ParseEntryHeader(ECRIREntryKind Kind, const FString& AfterKeyword, int32 LineNum,
    FCRIREntryBlock& OutBlock, TArray<FCRIRParseError>& OutErrors)
{
    OutBlock.Kind = Kind;
    OutBlock.SourceLine = LineNum;

    FString Working = AfterKeyword.TrimStartAndEnd();
    if (Kind == ECRIREntryKind::RigHierarchy)
    {
        if (!Working.IsEmpty())
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("'rig_hierarchy' header takes no arguments; got '%s'"), *Working),
                TEXT("CRIR_BAD_BLOCK_HEADER"));
            return false;
        }
        return true;
    }

    // RigGraph: optional model name. URigVMBlueprint default model is
    // "RigVMModel"; absent name means "use the asset's first model".
    // RigFunction: name is required.
    if (Working.IsEmpty())
    {
        if (Kind == ECRIREntryKind::RigFunction)
        {
            AddError(OutErrors, LineNum,
                TEXT("'rig_function' header requires a function-name token"),
                TEXT("CRIR_BAD_BLOCK_HEADER"));
            return false;
        }
        return true;
    }

    FString NameError;
    if (!ReadLeadingNameToken(Working, OutBlock.Name, NameError))
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Invalid %s name: %s"), EntryKindToText(Kind), *NameError),
            TEXT("CRIR_BAD_BLOCK_HEADER"));
        return false;
    }
    if (!Working.TrimStartAndEnd().IsEmpty())
    {
        AddError(OutErrors, LineNum,
            FString::Printf(TEXT("Unexpected trailing text after %s header: '%s'"), EntryKindToText(Kind), *Working),
            TEXT("CRIR_BAD_BLOCK_HEADER"));
        return false;
    }
    return true;
}

// One frame of the brace-stack. Exactly one of Instructions / ElementAttributes
// is non-null per frame; ElementAttributes routes element-line trailing
// `{...}` sub-block key=value lines into the most recently parsed control
// element's Attributes map (ticket 4 / F-crir-control-mutation-write).
struct FParseFrame
{
    TArray<FCRIRInstruction>* Instructions = nullptr;
    TMap<FString, FString>* ElementAttributes = nullptr;
    ECRIREntryKind EntryKind = ECRIREntryKind::RigGraph;
    int32 OpenLine = -1;
    bool bIsEntry = false;
};
}  // namespace

bool FCRIRParser::Parse(
    FStringView Code,
    TArray<FCRIREntryBlock>& OutBlocks,
    TArray<FCRIRParseError>& OutErrors,
    bool bSkipReferenceValidation)
{
    OutBlocks.Reset();
    OutErrors.Reset();

    TArray<FString> Lines;
    FString CodeStr(Code.Len(), Code.GetData());
    CodeStr.ParseIntoArray(Lines, TEXT("\n"), false);

    // N-deep brace stack. Top-level frames route to FCRIREntryBlock::Instructions;
    // nested rig_subgraph frames route to an owning Collapse instruction's
    // Children array (ticket 4 will additionally use ElementAttributes-routed
    // frames for control-element trailing sub-blocks).
    TArray<FParseFrame> Stack;

    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const int32 LineNum = Index + 1;
        FString Line = StripTrailingComment(Lines[Index]).TrimStartAndEnd();
        if (Line.IsEmpty())
        {
            continue;
        }

        if (Line == TEXT("}"))
        {
            if (Stack.Num() == 0)
            {
                AddError(OutErrors, LineNum, TEXT("Unbalanced '}': no open block"),
                    TEXT("CRIR_UNCLOSED_BLOCK"));
                return false;
            }
            Stack.Pop();
            continue;
        }

        // A standalone `{` line inside a RigHierarchy frame opens a sub-block
        // attached to the most recently parsed element. This matches the
        // human-written form where the brace sits on its own line under the
        // `control "C" ...` head; the same-line `... {` form is consumed by
        // ConsumeTrailingOpenBrace when the element is parsed.
        if (Line == TEXT("{"))
        {
            if (Stack.Num() == 0)
            {
                AddError(OutErrors, LineNum, TEXT("Unbalanced '{': no open block"),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                return false;
            }
            FParseFrame& TopFrame = Stack.Last();
            if (!(TopFrame.bIsEntry && TopFrame.EntryKind == ECRIREntryKind::RigHierarchy))
            {
                AddError(OutErrors, LineNum,
                    TEXT("Unexpected '{' line outside rig_hierarchy element context"),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }
            if (OutBlocks.Num() == 0 || OutBlocks.Last().Elements.Num() == 0)
            {
                AddError(OutErrors, LineNum,
                    TEXT("'{' sub-block must follow an element line"),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }
            FParseFrame SubFrame;
            SubFrame.ElementAttributes = &OutBlocks.Last().Elements.Last().Attributes;
            SubFrame.EntryKind = TopFrame.EntryKind;
            SubFrame.OpenLine = LineNum;
            SubFrame.bIsEntry = false;
            Stack.Add(SubFrame);
            continue;
        }

        FIrToken FirstToken;
        if (!TryReadFirstToken(Line, FirstToken))
        {
            continue;
        }

        // Top-level block-opener routing.
        if (Stack.Num() == 0)
        {
            if (FirstToken.Type != EIrTokenType::Keyword)
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Expected 'rig_graph', 'rig_hierarchy', or 'rig_function' at top level, got '%s'"),
                        *FirstToken.Text),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }

            const FString Lower = FirstToken.Text.ToLower();
            ECRIREntryKind Kind;
            if (Lower == TEXT("rig_graph"))
            {
                Kind = ECRIREntryKind::RigGraph;
            }
            else if (Lower == TEXT("rig_hierarchy"))
            {
                Kind = ECRIREntryKind::RigHierarchy;
            }
            else if (Lower == TEXT("rig_function"))
            {
                Kind = ECRIREntryKind::RigFunction;
            }
            else
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Unknown top-level keyword '%s'; expected 'rig_graph', 'rig_hierarchy', or 'rig_function'"),
                        *FirstToken.Text),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }

            FString Header = Line;
            const bool bHasOpenBrace = ConsumeTrailingOpenBrace(Header);
            if (!bHasOpenBrace)
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("CRIR '%s' header must end with '{'"), *Lower),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }

            const FString AfterKeyword = Header.Mid(FirstToken.Position + FirstToken.Text.Len());

            FCRIREntryBlock NewBlock;
            NewBlock.SourceLine = LineNum;
            if (!ParseEntryHeader(Kind, AfterKeyword, LineNum, NewBlock, OutErrors))
            {
                continue;
            }

            OutBlocks.Add(MoveTemp(NewBlock));
            FParseFrame Frame;
            Frame.Instructions = &OutBlocks.Last().Instructions;
            Frame.EntryKind = Kind;
            Frame.OpenLine = LineNum;
            Frame.bIsEntry = true;
            Stack.Add(Frame);
            continue;
        }

        // Inside a block — route by frame kind.
        FParseFrame& CurrentFrame = Stack.Last();

        // ElementAttributes-routed frame: control element sub-block. Each line
        // is a `key=value` pair (or bare flag) appended to the previously
        // parsed element's Attributes map. Unknown shapes emit a soft warning
        // — sub-block tolerance matches MGIR's property-set drift policy.
        if (CurrentFrame.ElementAttributes != nullptr)
        {
            FString SubLine = Line;
            FString SubKey;
            FString SubValue;
            int32 SubCursor = 0;
            // Read key bareword
            while (SubCursor < SubLine.Len() && SubLine[SubCursor] != TEXT('=') && !FChar::IsWhitespace(SubLine[SubCursor]))
            {
                ++SubCursor;
            }
            SubKey = SubLine.Left(SubCursor);
            if (SubKey.IsEmpty())
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Empty key in control sub-block line: '%s'"), *Line),
                    TEXT("CRIR_CONTROL_BAD_SUBBLOCK_KEY"));
                continue;
            }
            // Skip whitespace, then `=` (or treat bare flag as `=true`)
            while (SubCursor < SubLine.Len() && FChar::IsWhitespace(SubLine[SubCursor]))
            {
                ++SubCursor;
            }
            if (SubCursor >= SubLine.Len() || SubLine[SubCursor] != TEXT('='))
            {
                // Bare flag without `=` — store as "true".
                CurrentFrame.ElementAttributes->Add(SubKey, TEXT("true"));
                continue;
            }
            ++SubCursor; // skip `=`
            while (SubCursor < SubLine.Len() && FChar::IsWhitespace(SubLine[SubCursor]))
            {
                ++SubCursor;
            }
            if (SubCursor >= SubLine.Len())
            {
                CurrentFrame.ElementAttributes->Add(SubKey, FString());
                continue;
            }

            const TCHAR FirstSub = SubLine[SubCursor];
            if (FirstSub == TEXT('"'))
            {
                int32 EndQ = INDEX_NONE;
                for (int32 Idx = SubCursor + 1; Idx < SubLine.Len(); ++Idx)
                {
                    if (SubLine[Idx] == TEXT('"') && FIrTextUtils::IsUnescapedQuote(SubLine, Idx))
                    {
                        EndQ = Idx;
                        break;
                    }
                }
                if (EndQ == INDEX_NONE)
                {
                    AddError(OutErrors, LineNum,
                        FString::Printf(TEXT("Unterminated string in control sub-block attribute '%s'"), *SubKey),
                        TEXT("CRIR_CONTROL_BAD_SUBBLOCK_KEY"));
                    continue;
                }
                SubValue = SubLine.Mid(SubCursor + 1, EndQ - SubCursor - 1);
            }
            else if (FirstSub == TEXT('(') || FirstSub == TEXT('[') || FirstSub == TEXT('{'))
            {
                const TCHAR CloseSub = (FirstSub == TEXT('(')) ? TEXT(')')
                    : (FirstSub == TEXT('[')) ? TEXT(']') : TEXT('}');
                const int32 EndSub = FindMatchingChar(SubLine, SubCursor, FirstSub, CloseSub);
                if (EndSub == INDEX_NONE)
                {
                    AddError(OutErrors, LineNum,
                        FString::Printf(TEXT("Unmatched '%c' in control sub-block attribute '%s'"), FirstSub, *SubKey),
                        TEXT("CRIR_CONTROL_BAD_SUBBLOCK_KEY"));
                    continue;
                }
                SubValue = SubLine.Mid(SubCursor, EndSub - SubCursor + 1);
            }
            else
            {
                // Bareword value — span until end of line (whitespace tail is trimmed by line-level trim already).
                SubValue = SubLine.Mid(SubCursor).TrimEnd();
            }
            CurrentFrame.ElementAttributes->Add(SubKey, SubValue);
            continue;
        }

        // RigHierarchy frames only appear at the top level (no nested hierarchy
        // bodies). Element-trailing-{} sub-blocks (ticket 4) sit *inside* a
        // RigHierarchy frame and push an ElementAttributes-routed sub-frame —
        // they re-enter this branch through Stack.Last() being the entry frame.
        if (CurrentFrame.bIsEntry && CurrentFrame.EntryKind == ECRIREntryKind::RigHierarchy)
        {
            if (FirstToken.Type != EIrTokenType::Keyword)
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Expected element kind keyword in rig_hierarchy, got '%s'"),
                        *FirstToken.Text),
                    TEXT("CRIR_BAD_ELEMENT"));
                continue;
            }
            const FString KindToken = FirstToken.Text.ToLower();
            ECRIRElementKind ElemKind;
            if (KindToken == TEXT("bone"))         { ElemKind = ECRIRElementKind::Bone; }
            else if (KindToken == TEXT("null"))    { ElemKind = ECRIRElementKind::Null; }
            else if (KindToken == TEXT("control")) { ElemKind = ECRIRElementKind::Control; }
            else if (KindToken == TEXT("socket"))  { ElemKind = ECRIRElementKind::Socket; }
            else if (KindToken == TEXT("curve"))   { ElemKind = ECRIRElementKind::Curve; }
            else
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Unknown rig_hierarchy element kind '%s'"), *FirstToken.Text),
                    TEXT("CRIR_BAD_ELEMENT"));
                continue;
            }

            FString Rest = Line.Mid(FirstToken.Position + FirstToken.Text.Len()).TrimStart();
            // Element-trailing `{` opens a sub-block routing further key=value
            // lines into this element's Attributes map. Currently only used by
            // `control` elements for the long-tail FRigControlSettings fields,
            // but accepted on any element kind (the compiler ignores unknown
            // sub-attrs on bone/null/socket).
            const bool bOpensSubBlock = ConsumeTrailingOpenBrace(Rest);
            FCRIRElementInstruction Elem;
            if (!ParseElementInstruction(ElemKind, Rest, LineNum, Elem, OutErrors))
            {
                continue;
            }
            OutBlocks.Last().Elements.Add(MoveTemp(Elem));
            if (bOpensSubBlock)
            {
                FParseFrame SubFrame;
                SubFrame.ElementAttributes = &OutBlocks.Last().Elements.Last().Attributes;
                SubFrame.EntryKind = CurrentFrame.EntryKind;
                SubFrame.OpenLine = LineNum;
                SubFrame.bIsEntry = false;
                Stack.Add(SubFrame);
            }
            continue;
        }

        // Graph-like frame (RigGraph entry, RigFunction entry, or a nested
        // rig_subgraph frame — all use Instructions-routed CRIR ops).
        if (CurrentFrame.Instructions == nullptr)
        {
            AddError(OutErrors, LineNum,
                TEXT("Internal: graph frame has no Instructions destination"),
                TEXT("CRIR_INTERNAL"));
            continue;
        }

        // `rig_subgraph "Name" {` — sibling block opener that pushes a new
        // frame routed at a synthetic Collapse instruction's Children array.
        // (The decompiler doesn't emit bare `rig_subgraph` outside of collapse
        // heads, but the syntax is accepted for hand-written CRIR.)
        if (FirstToken.Type == EIrTokenType::Keyword && FirstToken.Text.ToLower() == TEXT("rig_subgraph"))
        {
            FString Header = Line;
            const bool bHasOpenBrace = ConsumeTrailingOpenBrace(Header);
            if (!bHasOpenBrace)
            {
                AddError(OutErrors, LineNum,
                    TEXT("CRIR 'rig_subgraph' header must end with '{'"),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }
            FString AfterKeyword = Header.Mid(FirstToken.Position + FirstToken.Text.Len()).TrimStart();
            FString SubgraphName;
            FString NameError;
            if (!ReadLeadingNameToken(AfterKeyword, SubgraphName, NameError) || SubgraphName.IsEmpty())
            {
                AddError(OutErrors, LineNum,
                    FString::Printf(TEXT("Invalid rig_subgraph name: %s"), *NameError),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }

            FCRIRInstruction Collapse;
            Collapse.Opcode = ECRIROpcode::Collapse;
            Collapse.VarName = SubgraphName;
            Collapse.SourceLine = LineNum;
            CurrentFrame.Instructions->Add(MoveTemp(Collapse));

            FParseFrame Frame;
            Frame.Instructions = &CurrentFrame.Instructions->Last().Children;
            Frame.EntryKind = ECRIREntryKind::RigGraph;
            Frame.OpenLine = LineNum;
            Frame.bIsEntry = false;
            Stack.Add(Frame);
            continue;
        }

        // `comment "<text>" ...` — bare keyword, no LocalId.
        if (FirstToken.Type == EIrTokenType::Keyword && FirstToken.Text.ToLower() == TEXT("comment"))
        {
            FCRIRInstruction Instruction;
            Instruction.SourceLine = LineNum;
            TryExtractPosition(Line, Instruction.Position);

            // Re-tokenize after position strip.
            FIrToken Tok;
            if (!TryReadFirstToken(Line, Tok))
            {
                continue;
            }
            const FString Rest = Line.Mid(Tok.Position + Tok.Text.Len()).TrimStart();
            if (!ParseCommentInstruction(Rest, LineNum, Instruction, OutErrors))
            {
                continue;
            }
            CurrentFrame.Instructions->Add(MoveTemp(Instruction));
            continue;
        }

        // `exposed_pin <Name>: <direction> <CPPType> [object=<path>] [= <default>]`
        // — bare keyword, no LocalId, no position. Declares one entry of a
        // function's signature; only meaningful inside `rig_function` bodies
        // (compile-side rejects use on a top-level rig_graph).
        if (FirstToken.Type == EIrTokenType::Keyword
            && FirstToken.Text.ToLower() == TEXT("exposed_pin"))
        {
            FCRIRInstruction Instruction;
            Instruction.SourceLine = LineNum;
            const FString Rest = Line.Mid(FirstToken.Position + FirstToken.Text.Len()).TrimStart();
            if (!ParseExposedPinInstruction(Rest, LineNum, Instruction, OutErrors))
            {
                continue;
            }
            CurrentFrame.Instructions->Add(MoveTemp(Instruction));
            continue;
        }

        // `function_entry [(args)]` / `function_return [(args)]` — bare
        // keyword, no LocalId. Args may follow, then optional `@(x,y)`.
        if (FirstToken.Type == EIrTokenType::Keyword
            && (FirstToken.Text.ToLower() == TEXT("function_entry")
                || FirstToken.Text.ToLower() == TEXT("function_return")))
        {
            const bool bIsEntry = FirstToken.Text.ToLower() == TEXT("function_entry");
            FCRIRInstruction Instruction;
            Instruction.SourceLine = LineNum;
            TryExtractPosition(Line, Instruction.Position);

            FIrToken Tok;
            if (!TryReadFirstToken(Line, Tok))
            {
                continue;
            }
            const FString Rest = Line.Mid(Tok.Position + Tok.Text.Len()).TrimStart();
            const ECRIROpcode Op = bIsEntry ? ECRIROpcode::FunctionEntry : ECRIROpcode::FunctionReturn;
            const TCHAR* Keyword = bIsEntry ? TEXT("function_entry") : TEXT("function_return");
            if (!ParseFunctionInterfaceInstruction(Op, Keyword, Rest, LineNum, Instruction, OutErrors))
            {
                continue;
            }
            CurrentFrame.Instructions->Add(MoveTemp(Instruction));
            continue;
        }

        if (FirstToken.Type != EIrTokenType::PercentRef)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Expected '%%localId = unit|var|reroute|if|select|enum|invoke_entry|template|dispatch|collapse|function_ref ...', 'rig_subgraph ...', 'function_entry/function_return', 'exposed_pin ...', or 'comment ...' in graph, got '%s'"),
                    *FirstToken.Text),
                TEXT("CRIR_BAD_OPCODE"));
            continue;
        }

        // `%localId = <opcode> ...` form. Strip position, then re-tokenize.
        FCRIRInstruction Instruction;
        Instruction.SourceLine = LineNum;
        TryExtractPosition(Line, Instruction.Position);

        FIrToken AfterStripFirst;
        if (!TryReadFirstToken(Line, AfterStripFirst) || AfterStripFirst.Type != EIrTokenType::PercentRef)
        {
            AddError(OutErrors, LineNum,
                TEXT("Lost leading '%localId' after stripping position annotation"),
                TEXT("CRIR_BAD_OPCODE"));
            continue;
        }

        const FString After = Line.Mid(AfterStripFirst.Position + AfterStripFirst.Text.Len()).TrimStart();
        if (!After.StartsWith(TEXT("=")))
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Expected '=' after %s"), *AfterStripFirst.Text),
                TEXT("CRIR_BAD_OPCODE"));
            continue;
        }

        const FString ResultName = AfterStripFirst.Text.Mid(1);  // strip leading `%`
        FString Rest = After.Mid(1).TrimStart();

        // Detect block-opening trailing `{` for opcodes that open a sub-graph
        // (currently only `collapse`). Strip it from Rest so per-opcode parsers
        // see a clean head line.
        const bool bOpensBlock = ConsumeTrailingOpenBrace(Rest);

        // Peek opcode keyword to dispatch.
        const TArray<FIrToken> RestTokens = FIrTokenizer::Tokenize(Rest, FCRIRGrammar::Get());
        if (RestTokens.Num() == 0 || RestTokens[0].Type != EIrTokenType::Keyword)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Expected CRIR opcode after '%%%s ='"), *ResultName),
                TEXT("CRIR_BAD_OPCODE"));
            continue;
        }

        const FString OpcodeText = RestTokens[0].Text.ToLower();
        bool bDispatched = true;
        if (OpcodeText == TEXT("unit"))
        {
            if (!ParseUnitInstruction(ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("var"))
        {
            if (!ParseVarInstruction(ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("reroute"))
        {
            if (!ParseRerouteInstruction(ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("if"))
        {
            if (!ParseIfOrSelectInstruction(ECRIROpcode::If, ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("select"))
        {
            if (!ParseIfOrSelectInstruction(ECRIROpcode::Select, ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("enum"))
        {
            if (!ParseEnumInstruction(ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("invoke_entry"))
        {
            if (!ParseInvokeEntryInstruction(ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("template"))
        {
            if (!ParseTemplateOrDispatchInstruction(ECRIROpcode::Template, TEXT("template"),
                    ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("dispatch"))
        {
            if (!ParseTemplateOrDispatchInstruction(ECRIROpcode::Dispatch, TEXT("dispatch"),
                    ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else if (OpcodeText == TEXT("collapse"))
        {
            if (!ParseCollapseInstruction(ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
            if (!bOpensBlock)
            {
                AddError(OutErrors, LineNum,
                    TEXT("'collapse' head must end with '{' opening the sub-graph body"),
                    TEXT("CRIR_BAD_BLOCK_HEADER"));
                continue;
            }
        }
        else if (OpcodeText == TEXT("function_ref"))
        {
            if (!ParseFunctionRefInstruction(ResultName, Rest, LineNum, Instruction, OutErrors)) continue;
        }
        else
        {
            bDispatched = false;
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Unknown CRIR opcode '%s'"),
                    *RestTokens[0].Text),
                TEXT("CRIR_BAD_OPCODE"));
        }
        if (!bDispatched)
        {
            continue;
        }

        if (bOpensBlock && Instruction.Opcode != ECRIROpcode::Collapse)
        {
            AddError(OutErrors, LineNum,
                FString::Printf(TEXT("Opcode '%s' does not open a '{' block"), *OpcodeText),
                TEXT("CRIR_BAD_BLOCK_HEADER"));
            continue;
        }

        CurrentFrame.Instructions->Add(MoveTemp(Instruction));

        if (bOpensBlock)
        {
            FParseFrame Frame;
            Frame.Instructions = &CurrentFrame.Instructions->Last().Children;
            Frame.EntryKind = ECRIREntryKind::RigGraph;
            Frame.OpenLine = LineNum;
            Frame.bIsEntry = false;
            Stack.Add(Frame);
        }
    }

    if (Stack.Num() != 0)
    {
        AddError(OutErrors, Stack.Last().OpenLine,
            TEXT("Unclosed CRIR block at end of input"),
            TEXT("CRIR_UNCLOSED_BLOCK"));
        return false;
    }

    if (!bSkipReferenceValidation)
    {
        for (const FCRIREntryBlock& Block : OutBlocks)
        {
            ValidateBlockReferences(Block, OutErrors);
        }
    }

    return OutErrors.Num() == 0;
}
