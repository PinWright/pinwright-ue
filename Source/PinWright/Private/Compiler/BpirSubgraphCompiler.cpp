// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirSubgraphCompiler.cpp - Compiles BPIR with header declarations into a collapsed subgraph

#include "Compiler/BpirSubgraphCompiler.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/CodePinResolver.h"

#include "K2Node_Tunnel.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"

DEFINE_LOG_CATEGORY(LogBpirSubgraphCompiler);

// Impure opcode keywords that appear at the start of a BPIR body line (after optional %name = )
static const TArray<FString> ImpureOpcodeKeywords = {
    TEXT("call "),
    // `message ` is the interface-message form of `call` and is always impure —
    // UK2Node_Message::IsNodePure() is hardcoded false.
    TEXT("message "),
    TEXT("set "),
    TEXT("branch"),
    TEXT("foreach"),
    TEXT("while"),
    TEXT("switch"),
    TEXT("cast<"),
    TEXT("latent "),
    TEXT("macro "),
    TEXT("sequence("),
    TEXT("exec "),
    TEXT("call_dispatcher"),
    TEXT("bind_dispatcher"),
    TEXT("unbind_dispatcher"),
    TEXT("clear_dispatcher"),
    TEXT("field_notify_subscribe"),
    TEXT("field_notify_unsubscribe"),
    TEXT("return"),
};

FBpirSubgraphCompiler::FBpirSubgraphCompiler(UBlueprint* InBlueprint, UEdGraph* InBoundGraph,
                                             UK2Node_Tunnel* InEntryTunnel, UK2Node_Tunnel* InExitTunnel)
    : Blueprint(InBlueprint)
    , BoundGraph(InBoundGraph)
    , EntryTunnel(InEntryTunnel)
    , ExitTunnel(InExitTunnel)
{
}

// ----------------------------------------------------------------------------
// SplitHeaderAndBody
// ----------------------------------------------------------------------------

void FBpirSubgraphCompiler::SplitHeaderAndBody(const FString& FullText,
                                                TArray<FString>& OutHeaderLines,
                                                FString& OutBody)
{
    TArray<FString> AllLines;
    FullText.ParseIntoArrayLines(AllLines);

    int32 SeparatorIdx = INDEX_NONE;
    for (int32 i = 0; i < AllLines.Num(); ++i)
    {
        if (AllLines[i].TrimStartAndEnd() == TEXT("---"))
        {
            SeparatorIdx = i;
            break;
        }
    }

    if (SeparatorIdx == INDEX_NONE)
    {
        // No header — entire text is body
        OutBody = FullText;
        return;
    }

    for (int32 i = 0; i < SeparatorIdx; ++i)
    {
        OutHeaderLines.Add(AllLines[i]);
    }

    // Reconstruct body from lines after separator
    TArray<FString> BodyLines;
    for (int32 i = SeparatorIdx + 1; i < AllLines.Num(); ++i)
    {
        BodyLines.Add(AllLines[i]);
    }
    OutBody = FString::Join(BodyLines, TEXT("\n"));
}

// ----------------------------------------------------------------------------
// ParseDeclarations
// ----------------------------------------------------------------------------

bool FBpirSubgraphCompiler::ParseDeclarations(const TArray<FString>& HeaderLines,
                                               TArray<FBpirExpressionDecl>& OutDecls,
                                               TArray<FCompileError>& OutErrors)
{
    TSet<FString> SeenNames;

    for (int32 LineIdx = 0; LineIdx < HeaderLines.Num(); ++LineIdx)
    {
        FString Line = HeaderLines[LineIdx].TrimStartAndEnd();

        // Skip blank lines and comments
        if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("//")))
        {
            continue;
        }

        FBpirExpressionDecl Decl;
        bool bParsed = false;

        if (Line.StartsWith(TEXT("input "), ESearchCase::IgnoreCase))
        {
            Decl.bIsInput = true;
            FString Rest = Line.Mid(6).TrimStart();
            bParsed = true;

            // Check for default value: Name: Type = Default
            FString NameAndType;
            if (Rest.Split(TEXT("="), &NameAndType, &Decl.DefaultValue))
            {
                NameAndType.TrimEndInline();
                Decl.DefaultValue.TrimStartAndEndInline();
            }
            else
            {
                NameAndType = Rest;
            }

            // Split Name: Type
            FString NamePart;
            FString TypeSrc;
            if (NameAndType.Split(TEXT(":"), &NamePart, &TypeSrc))
            {
                Decl.Name = NamePart.TrimStartAndEnd();
                TypeSrc.TrimStartAndEndInline();
                if (!TypeSrc.IsEmpty())
                {
                    FString ParseErr;
                    int32 ParseErrCol = INDEX_NONE;
                    if (!BpirTypeSpecParser::ParseTypeSpec(TypeSrc, Decl.TypeSpec, ParseErr, ParseErrCol))
                    {
                        OutErrors.Add(FCompileError(LineIdx + 1,
                            FString::Printf(TEXT("Invalid type '%s' in input '%s': %s"),
                                *TypeSrc, *Decl.Name,
                                *BpirTypeSpecParser::FormatTypeSpecErrorDetail(ParseErr, ParseErrCol))));
                        // Leave TypeSpec default-constructed — treated as wildcard below.
                        Decl.TypeSpec = FBpirTypeSpec{};
                    }
                }
            }
            else
            {
                Decl.Name = NameAndType.TrimStartAndEnd();
                // No type specified — wildcard
            }
        }
        else if (Line.StartsWith(TEXT("output "), ESearchCase::IgnoreCase))
        {
            Decl.bIsInput = false;
            FString Rest = Line.Mid(7).TrimStart();
            bParsed = true;

            // Split Name: Type (no defaults for outputs)
            FString NamePart;
            FString TypeSrc;
            if (Rest.Split(TEXT(":"), &NamePart, &TypeSrc))
            {
                Decl.Name = NamePart.TrimStartAndEnd();
                TypeSrc.TrimStartAndEndInline();
                if (!TypeSrc.IsEmpty())
                {
                    FString ParseErr;
                    int32 ParseErrCol = INDEX_NONE;
                    if (!BpirTypeSpecParser::ParseTypeSpec(TypeSrc, Decl.TypeSpec, ParseErr, ParseErrCol))
                    {
                        OutErrors.Add(FCompileError(LineIdx + 1,
                            FString::Printf(TEXT("Invalid output type '%s' in output '%s': %s"),
                                *TypeSrc, *Decl.Name,
                                *BpirTypeSpecParser::FormatTypeSpecErrorDetail(ParseErr, ParseErrCol))));
                        Decl.TypeSpec = FBpirTypeSpec{};
                    }
                }
            }
            else
            {
                Decl.Name = Rest.TrimStartAndEnd();
            }
        }

        if (!bParsed)
        {
            OutErrors.Add(FCompileError(LineIdx + 1,
                FString::Printf(TEXT("Unrecognized header line: '%s'"), *Line)));
            return false;
        }

        if (Decl.Name.IsEmpty())
        {
            OutErrors.Add(FCompileError(LineIdx + 1, TEXT("Declaration has empty name")));
            return false;
        }

        if (SeenNames.Contains(Decl.Name))
        {
            OutErrors.Add(FCompileError(LineIdx + 1,
                FString::Printf(TEXT("Duplicate declaration name: '%s'"), *Decl.Name)));
            return false;
        }

        SeenNames.Add(Decl.Name);
        OutDecls.Add(MoveTemp(Decl));
    }

    return true;
}

// ----------------------------------------------------------------------------
// AutoDiscoverInputs
// ----------------------------------------------------------------------------

void FBpirSubgraphCompiler::AutoDiscoverInputs(const FString& Body,
                                                TArray<FBpirExpressionDecl>& OutDecls)
{
    TSet<FString> FoundVars;

    // Scan for $VarName patterns (word characters after $)
    int32 Pos = 0;
    while (Pos < Body.Len())
    {
        int32 DollarIdx = Body.Find(TEXT("$"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
        if (DollarIdx == INDEX_NONE)
        {
            break;
        }

        // Extract variable name (alphanumeric + underscore)
        int32 NameStart = DollarIdx + 1;
        int32 NameEnd = NameStart;
        while (NameEnd < Body.Len())
        {
            TCHAR Ch = Body[NameEnd];
            if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
            {
                ++NameEnd;
            }
            else
            {
                break;
            }
        }

        if (NameEnd > NameStart)
        {
            // The extracted name is always the root (dot stops extraction),
            // so $Target.Property yields just "Target" here
            FString RootName = Body.Mid(NameStart, NameEnd - NameStart);

            if (!FoundVars.Contains(RootName))
            {
                FoundVars.Add(RootName);

                // Check if this is an existing blueprint variable — if so, skip
                // (it will be resolved by PreEmitDollarVar as a VariableGet)
                bool bIsBlueprintVar = false;
                if (Blueprint && Blueprint->GeneratedClass)
                {
                    FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(FName(*RootName));
                    if (Prop)
                    {
                        bIsBlueprintVar = true;
                    }
                }

                if (!bIsBlueprintVar)
                {
                    FBpirExpressionDecl Decl;
                    Decl.Name = RootName;
                    Decl.bIsInput = true;
                    // TypeSpec left default — wildcard
                    OutDecls.Add(MoveTemp(Decl));
                }
            }
        }

        Pos = NameEnd;
    }
}

// ----------------------------------------------------------------------------
// DetectImpure
// ----------------------------------------------------------------------------

bool FBpirSubgraphCompiler::DetectImpure(const FString& Body)
{
    TArray<FString> Lines;
    Body.ParseIntoArrayLines(Lines);

    for (const FString& RawLine : Lines)
    {
        FString Line = RawLine.TrimStart();

        // Skip empty lines and comments
        if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("//")))
        {
            continue;
        }

        // Strip optional %name = prefix to get the opcode portion
        FString OpcodeStr = Line;
        int32 EqIdx = Line.Find(TEXT("="));
        if (EqIdx != INDEX_NONE && Line.StartsWith(TEXT("%")))
        {
            // Verify this is %name = ... (not part of a default value)
            FString BeforeEq = Line.Left(EqIdx).TrimEnd();
            if (!BeforeEq.Contains(TEXT(" ")) || BeforeEq.StartsWith(TEXT("%")))
            {
                OpcodeStr = Line.Mid(EqIdx + 1).TrimStart();
            }
        }

        for (const FString& Keyword : ImpureOpcodeKeywords)
        {
            if (OpcodeStr.StartsWith(Keyword, ESearchCase::CaseSensitive))
            {
                return true;
            }
        }
    }

    return false;
}

// ----------------------------------------------------------------------------
// CompileIntoSubgraph
// ----------------------------------------------------------------------------

FCompileResult FBpirSubgraphCompiler::CompileIntoSubgraph(const FString& FullBpirText)
{
    if (!Blueprint || !BoundGraph || !EntryTunnel || !ExitTunnel)
    {
        return FCompileResult::MakeError(-1, TEXT("BpirSubgraphCompiler: null Blueprint, BoundGraph, or tunnel node."));
    }

    // Step 1: Split header and body
    TArray<FString> HeaderLines;
    FString Body;
    SplitHeaderAndBody(FullBpirText, HeaderLines, Body);

    if (Body.TrimStartAndEnd().IsEmpty())
    {
        return FCompileResult::MakeError(-1, TEXT("BpirSubgraphCompiler: empty body."));
    }

    // Step 2: Parse declarations
    TArray<FBpirExpressionDecl> Decls;
    TArray<FCompileError> ParseErrors;
    bool bHasHeader = HeaderLines.Num() > 0;

    if (bHasHeader)
    {
        if (!ParseDeclarations(HeaderLines, Decls, ParseErrors))
        {
            FCompileResult ErrorResult;
            ErrorResult.bSuccess = false;
            ErrorResult.Errors = ParseErrors;
            return ErrorResult;
        }
    }

    // Step 3: Auto-discover inputs when no header
    if (!bHasHeader)
    {
        AutoDiscoverInputs(Body, Decls);
    }

    // Step 4: Determine if expression is impure
    bool bIsImpure = DetectImpure(Body);

    // Step 5: Configure entry tunnel pins
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

    if (bIsImpure)
    {
        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        EntryTunnel->CreateUserDefinedPin(UEdGraphSchema_K2::PN_Execute, ExecPinType, EGPD_Output);
    }

    for (const FBpirExpressionDecl& Decl : Decls)
    {
        if (!Decl.bIsInput)
        {
            continue;
        }

        FEdGraphPinType PinType;
        if (!Decl.TypeSpec.IsEmpty())
        {
            FCodePinResolver::ConvertTypeSpecToPinType(Decl.TypeSpec, PinType);
        }
        else
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }

        UEdGraphPin* CreatedPin = EntryTunnel->CreateUserDefinedPin(
            FName(*Decl.Name), PinType, EGPD_Output);

        if (CreatedPin && !Decl.DefaultValue.IsEmpty())
        {
            CreatedPin->DefaultValue = Decl.DefaultValue;
        }
    }

    // Step 6: Configure exit tunnel pins
    if (bIsImpure)
    {
        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        ExitTunnel->CreateUserDefinedPin(UEdGraphSchema_K2::PN_Execute, ExecPinType, EGPD_Input);
    }

    for (const FBpirExpressionDecl& Decl : Decls)
    {
        if (Decl.bIsInput)
        {
            continue;
        }

        FEdGraphPinType PinType;
        if (!Decl.TypeSpec.IsEmpty())
        {
            FCodePinResolver::ConvertTypeSpecToPinType(Decl.TypeSpec, PinType);
        }
        else
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }

        ExitTunnel->CreateUserDefinedPin(FName(*Decl.Name), PinType, EGPD_Input);
    }

    // Step 7: Create compiler and inject input variables
    FBpirCompiler Compiler(Blueprint);

    // Set exit tunnel so `return` wires to it (composite subgraph acts like macro)
    Compiler.SetExitTunnel(ExitTunnel);

    for (const FBpirExpressionDecl& Decl : Decls)
    {
        if (!Decl.bIsInput)
        {
            continue;
        }

        // Find the matching output pin on entry tunnel by name
        UEdGraphPin* TunnelPin = EntryTunnel->FindPin(FName(*Decl.Name), EGPD_Output);
        if (TunnelPin)
        {
            Compiler.InjectExternalVariable(Decl.Name, TunnelPin);
        }
        else
        {
            UE_LOG(LogBpirSubgraphCompiler, Warning,
                TEXT("Could not find entry tunnel output pin for input '%s'"), *Decl.Name);
        }
    }

    // Step 8: Find entry exec pin
    UEdGraphPin* EntryExecPin = nullptr;
    if (bIsImpure)
    {
        EntryExecPin = EntryTunnel->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Output);
    }

    // Step 9: Compile body
    FCompileResult Result = Compiler.CompileBodyIntoGraph(Body, BoundGraph, EntryExecPin);
    if (!Result.bSuccess)
    {
        return Result;
    }

    // Step 10: Wire outputs to exit tunnel
    // Parse the body separately to get the ValueIndex for output name -> instruction mapping
    TArray<FBpirExpressionDecl> OutputDecls;
    for (const FBpirExpressionDecl& Decl : Decls)
    {
        if (!Decl.bIsInput)
        {
            OutputDecls.Add(Decl);
        }
    }

    if (OutputDecls.Num() > 0)
    {
        FBpirEntryBlock Block;
        TArray<FCompileError> BodyParseErrors;
        FBpirParser Parser;

        if (Parser.ParseBody(Body, Block, BodyParseErrors))
        {
            const TMap<int32, FEmittedNodeInfo>& EmitMap = Compiler.GetEmitMap();

            for (const FBpirExpressionDecl& OutputDecl : OutputDecls)
            {
                const int32* InstructionIdx = Block.ValueIndex.Find(OutputDecl.Name);
                if (!InstructionIdx)
                {
                    UE_LOG(LogBpirSubgraphCompiler, Warning,
                        TEXT("Output '%s' has no matching %%ref in body"), *OutputDecl.Name);
                    continue;
                }

                const FEmittedNodeInfo* NodeInfo = EmitMap.Find(*InstructionIdx);
                if (!NodeInfo || !NodeInfo->PrimaryOutputPin)
                {
                    UE_LOG(LogBpirSubgraphCompiler, Warning,
                        TEXT("Output '%s': no emitted node found for instruction %d"), *OutputDecl.Name, *InstructionIdx);
                    continue;
                }

                UEdGraphPin* ExitPin = ExitTunnel->FindPin(FName(*OutputDecl.Name), EGPD_Input);
                if (!ExitPin)
                {
                    UE_LOG(LogBpirSubgraphCompiler, Warning,
                        TEXT("Output '%s': no matching pin on exit tunnel"), *OutputDecl.Name);
                    continue;
                }

                // If output type was wildcard, adopt the type from the source pin
                if (ExitPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
                {
                    ExitPin->PinType = NodeInfo->PrimaryOutputPin->PinType;
                }

                Schema->TryCreateConnection(NodeInfo->PrimaryOutputPin, ExitPin);
            }
        }
        else
        {
            UE_LOG(LogBpirSubgraphCompiler, Warning,
                TEXT("Failed to re-parse body for output wiring (this should not happen)"));
        }
    }

    // Step 11: Wire exit exec
    if (bIsImpure && Result.LastExecOutputPin)
    {
        UEdGraphPin* ExitExecPin = ExitTunnel->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (ExitExecPin)
        {
            Schema->TryCreateConnection(Result.LastExecOutputPin, ExitExecPin);
        }
    }

    // Step 12: Return result
    return Result;
}
