// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirValueResolver.cpp - Resolves BPIR value references to UEdGraphPins

#include "Compiler/BpirValueResolver.h"
#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/CodeNodeEmitter.h"
#include "Compiler/CodePinResolver.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "IrCore/IrTextUtils.h"
#include "Utils/ClassUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_VariableGet.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Self.h"

DEFINE_LOG_CATEGORY_STATIC(LogBpirValueResolver, Log, All);

namespace
{
    FString NormalizeBpirNameToken(const FString& Text)
    {
        FString Name;
        FString Error;
        if (FIrTextUtils::TryUnwrapNameToken(Text, Name, Error))
        {
            return Name;
        }
        return Text.TrimStartAndEnd();
    }

    TArray<FString> SplitNormalizedBpirPropertyPath(const FString& PropertyPath)
    {
        TArray<FString> Segments;
        const FString TrimmedPropertyPath = PropertyPath.TrimStartAndEnd();
        const TArray<int32> DotPositions = FIrTextUtils::FindTopLevelDelimiterPositions(TrimmedPropertyPath, TEXT('.'), true);

        int32 SegmentStart = 0;
        for (const int32 DotIndex : DotPositions)
        {
            Segments.Add(NormalizeBpirNameToken(TrimmedPropertyPath.Mid(SegmentStart, DotIndex - SegmentStart)));
            SegmentStart = DotIndex + 1;
        }

        Segments.Add(NormalizeBpirNameToken(TrimmedPropertyPath.Mid(SegmentStart)));
        return Segments;
    }

    FString NormalizeBpirPropertyPath(const FString& PropertyPath)
    {
        return FString::Join(SplitNormalizedBpirPropertyPath(PropertyPath), TEXT("."));
    }

    bool SplitDollarReference(const FString& Reference, FString& OutTargetName, FString& OutPropertyName)
    {
        const TArray<int32> DotPositions = FIrTextUtils::FindTopLevelDelimiterPositions(Reference, TEXT('.'), true);
        if (DotPositions.Num() == 0)
        {
            OutTargetName = NormalizeBpirNameToken(Reference);
            OutPropertyName.Reset();
            return false;
        }

        const int32 DotIndex = DotPositions[0];
        OutTargetName = NormalizeBpirNameToken(Reference.Left(DotIndex));
        OutPropertyName = NormalizeBpirPropertyPath(Reference.Mid(DotIndex + 1));
        return true;
    }

    UEnum* ResolveEnumTypeFromLiteralText(const FString& ValueText)
    {
        FString EnumName;
        FString ValueName;
        if (ValueText.Split(TEXT("::"), &EnumName, &ValueName))
        {
            return ResolveUEnum(EnumName);
        }
        return nullptr;
    }

    UEnum* ResolveEnumTypeFromValueRefInternal(
        const FString& ValueRef,
        const FBpirEntryBlock& Block,
        TSet<FString>& VisitingRefs)
    {
        if (UEnum* EnumType = ResolveEnumTypeFromLiteralText(ValueRef))
        {
            return EnumType;
        }

        if (!ValueRef.StartsWith(TEXT("%")))
        {
            return nullptr;
        }

        FString RefName = ValueRef.Mid(1);
        int32 DotIdx = INDEX_NONE;
        if (RefName.FindChar(TEXT('.'), DotIdx))
        {
            RefName = RefName.Left(DotIdx);
        }

        if (VisitingRefs.Contains(RefName))
        {
            return nullptr;
        }
        VisitingRefs.Add(RefName);

        const int32* RefIdx = Block.ValueIndex.Find(RefName);
        if (!RefIdx || !Block.Instructions.IsValidIndex(*RefIdx))
        {
            return nullptr;
        }

        const FBpirInstruction& RefInst = Block.Instructions[*RefIdx];
        if (RefInst.Opcode == EBpirOpcode::Enum)
        {
            return ResolveEnumTypeFromLiteralText(RefInst.TypeArg);
        }
        if (RefInst.Opcode == EBpirOpcode::Alias)
        {
            return ResolveEnumTypeFromValueRefInternal(RefInst.AliasRhs, Block, VisitingRefs);
        }
        return nullptr;
    }
}

FBpirValueResolver::FBpirValueResolver(FCodeNodeEmitter& InNodeEmitter, FCodePinResolver& InPinResolver,
                                       UBlueprint* InBlueprint, UEdGraph* InGraph)
    : NodeEmitter(InNodeEmitter)
    , PinResolver(InPinResolver)
    , Blueprint(InBlueprint)
    , Graph(InGraph)
{
}

UEdGraphPin* FBpirValueResolver::ResolveValue(const FString& ValueRef, FBpirEntryBlock& Block)
{
    if (ValueRef.IsEmpty())
    {
        return nullptr;
    }

    // %name or %name.PinName reference
    if (ValueRef.StartsWith(TEXT("%")))
    {
        const FString Remainder = ValueRef.Mid(1);

        // Check for dot-separated pin access: %name.PinName. Split on the first
        // top-level dot so a backtick-quoted segment keeps its own dots, then drop
        // the quoting: a spaced pin name is spelled %loop.`Array Element` (what the
        // decompiler emits), while pin lookup compares against bare engine pin names.
        const TArray<int32> DotPositions = FIrTextUtils::FindTopLevelDelimiterPositions(Remainder, TEXT('.'), true);
        if (DotPositions.Num() > 0)
        {
            const int32 DotIndex = DotPositions[0];
            const FString Name = Remainder.Left(DotIndex);
            const FString PinName = NormalizeBpirPropertyPath(Remainder.Mid(DotIndex + 1));
            return ResolvePercentRefPin(Name, PinName, Block);
        }

        return ResolvePercentRef(Remainder, Block);
    }

    // $varName or $target.Property reference
    if (ValueRef.StartsWith(TEXT("$")))
    {
        const FString VarRef = ValueRef.Mid(1);

        // External property access: $target.Property
        FString TargetName;
        FString PropertyName;
        if (SplitDollarReference(VarRef, TargetName, PropertyName))
        {
            FString Key = TargetName + TEXT(".") + PropertyName;
            if (UEdGraphPin** CachedPin = ExternalGetCache.Find(Key))
            {
                return *CachedPin;
            }
            UE_LOG(LogBpirValueResolver, Error, TEXT("External property reference '$%s' not found — was PreEmitExternalGet called?"), *Key);
            return nullptr;
        }

        const FString NormalizedName = NormalizeBpirNameToken(TargetName);
        if (UEdGraphPin* ResolvedPin = PinResolver.ResolveVariable(NormalizedName))
        {
            return ResolvedPin;
        }
        if (UEdGraphPin** CachedPin = VariableGetCache.Find(NormalizedName))
        {
            return *CachedPin;
        }

        // Ergonomic fallback: when no blueprint variable of this name exists but
        // a local %name register does, treat $name as that register. Lets authors
        // write `$v` interchangeably with `%v` when the intent is unambiguous.
        if (Block.ValueIndex.Contains(NormalizedName))
        {
            return ResolvePercentRef(NormalizedName, Block);
        }

        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveDollarVar: '$%s' not found — was PreEmitDollarVar called?"), *NormalizedName);
        return nullptr;
    }

    // self reference
    if (ValueRef == TEXT("self"))
    {
        return ResolveSelf();
    }

    // Inline `cast<T>(inner)` expression — emits a pure K2Node_DynamicCast
    // and returns its result pin. The standalone `cast` statement path
    // (impure cast with explicit success/fail labels) is handled separately
    // by the opcode emitter and is unaffected by this branch.
    if (ValueRef.StartsWith(TEXT("cast<")))
    {
        return ResolveCastExpression(ValueRef, Block);
    }

    // Literal values are not resolved to pins — caller should use SetPinDefaultValue
    if (IsLiteral(ValueRef))
    {
        return nullptr;
    }

    // Fallback: try PinResolver for registered parameters (custom event params, function params).
    // This allows bare parameter names (e.g., "ArmState") without requiring the $ prefix.
    if (UEdGraphPin* ParamPin = PinResolver.ResolveVariable(ValueRef))
    {
        return ParamPin;
    }

    UE_LOG(LogBpirValueResolver, Warning, TEXT("Unknown value reference format: '%s'"), *ValueRef);
    return nullptr;
}

UEdGraphPin* FBpirValueResolver::ResolvePercentRef(const FString& Name, FBpirEntryBlock& Block)
{
    const int32* IndexPtr = Block.ValueIndex.Find(Name);
    if (!IndexPtr)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s' not found in block value index"), *Name);
        return nullptr;
    }

    int32 Index = *IndexPtr;
    if (!Block.Instructions.IsValidIndex(Index))
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s' maps to invalid instruction index %d"), *Name, Index);
        return nullptr;
    }

    if (!EmitMap)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s' — EmitMap not set"), *Name);
        return nullptr;
    }

    const FEmittedNodeInfo* Info = EmitMap->Find(Index);
    if (!Info || !Info->PrimaryOutputPin)
    {
        // Enum/Label/Comment/ExecGoto instructions intentionally have no emitted node.
        // Return nullptr silently — the caller (WireDataPins) handles enum %refs
        // by falling through to literal resolution.
        const FBpirInstruction& RefInst = Block.Instructions[Index];
        if (RefInst.Opcode == EBpirOpcode::Enum
            || RefInst.Opcode == EBpirOpcode::Label
            || RefInst.Opcode == EBpirOpcode::Comment
            || RefInst.Opcode == EBpirOpcode::ExecGoto)
        {
            return nullptr;
        }

        // Alias instructions have no emitted node — resolve by delegating to the RHS.
        // Guard against cycles like `%a = %b; %b = %a` which would otherwise recurse forever.
        if (RefInst.Opcode == EBpirOpcode::Alias)
        {
            const FString& RhsRef = RefInst.AliasRhs;
            if (RhsRef.StartsWith(TEXT("$")))
            {
                constexpr int32 MaxAliasDepth = 16;
                if (AliasDepth >= MaxAliasDepth)
                {
                    UE_LOG(LogBpirValueResolver, Warning, TEXT("Alias '%%%s': chain exceeds max depth %d (possible cycle)"), *Name, MaxAliasDepth);
                    return nullptr;
                }
                TGuardValue<int32> DepthGuard(AliasDepth, AliasDepth + 1);
                return ResolveValue(RhsRef, Block);
            }
            if (RhsRef.StartsWith(TEXT("%")))
            {
                constexpr int32 MaxAliasDepth = 16;
                if (AliasDepth >= MaxAliasDepth)
                {
                    UE_LOG(LogBpirValueResolver, Warning, TEXT("Alias '%%%s': chain exceeds max depth %d (possible cycle)"), *Name, MaxAliasDepth);
                    return nullptr;
                }
                TGuardValue<int32> DepthGuard(AliasDepth, AliasDepth + 1);
                return ResolvePercentRef(RhsRef.Mid(1), Block);
            }
            UE_LOG(LogBpirValueResolver, Error, TEXT("Alias '%%%s': unrecognized RHS format '%s'"), *Name, *RhsRef);
            return nullptr;
        }

        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s' instruction has no primary output pin"), *Name);
        return nullptr;
    }

    return Info->PrimaryOutputPin;
}

UEdGraphPin* FBpirValueResolver::ResolvePercentRefPin(const FString& Name, const FString& PinName, FBpirEntryBlock& Block)
{
    const int32* IndexPtr = Block.ValueIndex.Find(Name);
    if (!IndexPtr)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s.%s' — name not found in block value index"), *Name, *PinName);
        return nullptr;
    }

    int32 Index = *IndexPtr;
    if (!Block.Instructions.IsValidIndex(Index))
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s.%s' maps to invalid instruction index %d"), *Name, *PinName, Index);
        return nullptr;
    }

    if (!EmitMap)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s.%s' — EmitMap not set"), *Name, *PinName);
        return nullptr;
    }

    const FEmittedNodeInfo* Info = EmitMap->Find(Index);
    UEdGraphNode* Node = Info ? Info->Node : nullptr;
    if (!Node)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s.%s' — instruction has no emitted node"), *Name, *PinName);
        return nullptr;
    }

    UEdGraphPin* FoundPin = FindOutputPinByName(Node, PinName);
    if (FoundPin)
    {
        return FoundPin;
    }

    // Multi-dot fallback: if PinName contains a dot, interpret as chained property access
    // e.g., "AsActor.bHidden" -> first segment is the pin name, rest are property chain
    int32 DotIndex = INDEX_NONE;
    if (PinName.FindChar(TEXT('.'), DotIndex))
    {
        FString FirstSegment = PinName.Left(DotIndex);
        FString Remainder = PinName.Mid(DotIndex + 1);

        // Build cache key from node pointer + full PinName
        FString CacheKey = FString::Printf(TEXT("%p_%s"), Node, *PinName);
        if (UEdGraphPin** CachedPin = AutoPropertyGetCache.Find(CacheKey))
        {
            return *CachedPin;
        }

        // Parse remaining segments
        TArray<FString> PropertyChain;
        Remainder.ParseIntoArray(PropertyChain, TEXT("."));

        UEdGraphPin* ChainResult = ResolveChainedPropertyAccess(Node, FirstSegment, PropertyChain);
        if (ChainResult)
        {
            AutoPropertyGetCache.Add(CacheKey, ChainResult);
            return ChainResult;
        }
    }

    // Struct ReturnValue fallback: if PinName has no dot and the node has a struct-typed
    // ReturnValue pin, auto-break the struct and find the member by name.
    // e.g., %loc.X where %loc = GetActorForwardVector() returning FVector
    if (DotIndex == INDEX_NONE)
    {
        // Look for a struct-typed ReturnValue output pin
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
            {
                UEdGraphPin* MemberPin = ResolveStructMemberThroughPin(Pin, PinName);
                if (MemberPin)
                {
                    return MemberPin;
                }
                break;
            }
        }
    }

    // Object property access fallback: if the node has an object-typed output pin
    // (ReturnValue preferred, then any object output), delegate to ResolveChainedPropertyAccess
    // which handles create/wire/cache for ExternalVariableGet nodes.
    // e.g., %n0.Health where %n0 = GetRelevantActor() returning AActor*
    if (DotIndex == INDEX_NONE)
    {
        // Single pass: prefer ReturnValue, fall back to first object-typed output
        UEdGraphPin* ObjectPin = nullptr;
        UEdGraphPin* FallbackPin = nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction != EGPD_Output) continue;
            if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Object
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Interface) continue;
            if (Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue) { ObjectPin = Pin; break; }
            if (!FallbackPin) { FallbackPin = Pin; }
        }
        if (!ObjectPin) { ObjectPin = FallbackPin; }

        if (ObjectPin)
        {
            // Delegate to ResolveChainedPropertyAccess which handles the
            // ExternalVariableGet create/wire/cache pattern for object pins.
            TArray<FString> Chain;
            Chain.Add(PinName);
            UEdGraphPin* Result = ResolveChainedPropertyAccess(Node, ObjectPin->PinName.ToString(), Chain);
            if (Result)
            {
                return Result;
            }
        }
    }

    UE_LOG(LogBpirValueResolver, Error, TEXT("Value reference '%%%s.%s' — no output pin matching '%s' on node '%s'"),
        *Name, *PinName, *PinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    return nullptr;
}

void FBpirValueResolver::PreEmitDollarVar(const FString& VarName)
{
    const FString NormalizedVarName = NormalizeBpirNameToken(VarName);

    // Skip if PinResolver already handles it (function/event parameters)
    if (PinResolver.ResolveVariable(NormalizedVarName))
    {
        return;
    }

    // Skip if already cached
    if (VariableGetCache.Contains(NormalizedVarName))
    {
        return;
    }

    if (MissingVariableGetCache.Contains(NormalizedVarName))
    {
        return;
    }

    if (!BlueprintHandlerUtils::DoesGraphVariableExist(Blueprint, nullptr, FName(*NormalizedVarName)))
    {
        MissingVariableGetCache.Add(NormalizedVarName);
        return;
    }

    UK2Node_VariableGet* VarGetNode = NodeEmitter.CreateVariableGetNode(FName(*NormalizedVarName));
    if (!VarGetNode)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitDollarVar: Failed to create VariableGet node for '$%s'"), *NormalizedVarName);
        return;
    }

    UEdGraphPin* ValuePin = FindFirstDataOutputPin(VarGetNode);
    if (!ValuePin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitDollarVar: VariableGet node for '$%s' has no non-exec output pin"), *NormalizedVarName);
        return;
    }

    VariableGetCache.Add(NormalizedVarName, ValuePin);
    PinResolver.RegisterVariable(NormalizedVarName, ValuePin);
}

UEdGraphPin* FBpirValueResolver::ResolveDollarVar(const FString& VarName)
{
    const FString NormalizedVarName = NormalizeBpirNameToken(VarName);

    // Check PinResolver first (handles function/event parameters)
    if (UEdGraphPin* ResolvedPin = PinResolver.ResolveVariable(NormalizedVarName))
    {
        return ResolvedPin;
    }

    // Pure lookup from cache — node should have been pre-emitted
    if (UEdGraphPin** CachedPin = VariableGetCache.Find(NormalizedVarName))
    {
        return *CachedPin;
    }

    UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveDollarVar: '$%s' not found — was PreEmitDollarVar called?"), *NormalizedVarName);
    return nullptr;
}

void FBpirValueResolver::PreEmitSelf()
{
    if (CachedSelfPin)
    {
        return;
    }

    UK2Node_Self* SelfNode = NodeEmitter.CreateSelfNode();
    if (!SelfNode)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitSelf: Failed to create Self node"));
        return;
    }

    CachedSelfPin = FindFirstDataOutputPin(SelfNode);
    if (!CachedSelfPin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitSelf: Self node has no output pin"));
    }
}

UEdGraphPin* FBpirValueResolver::ResolveSelf()
{
    if (CachedSelfPin)
    {
        return CachedSelfPin;
    }

    UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveSelf: Self node not found — was PreEmitSelf called?"));
    return nullptr;
}

bool FBpirValueResolver::ParseCastSyntax(const FString& Expr, FString& OutType, FString& OutInner)
{
    // Expected shape: cast<T>(inner). T may contain dots / slashes (UE classpath);
    // inner may itself contain nested cast<...>(...) so angle/paren matching honors depth.
    constexpr int32 CastPrefixLen = 5; // length of "cast<"
    if (!Expr.StartsWith(TEXT("cast<")) || Expr.Len() <= CastPrefixLen)
    {
        return false;
    }

    const int32 OpenAngle = CastPrefixLen - 1;
    const int32 CloseAngle = FIrTextUtils::FindMatchingChar(Expr, OpenAngle, TEXT('<'), TEXT('>'));
    if (CloseAngle == INDEX_NONE)
    {
        return false;
    }

    FString TypeName = Expr.Mid(CastPrefixLen, CloseAngle - CastPrefixLen).TrimStartAndEnd();
    if (TypeName.IsEmpty())
    {
        return false;
    }

    if (CloseAngle + 1 >= Expr.Len() || Expr[CloseAngle + 1] != TEXT('('))
    {
        return false;
    }

    const int32 OpenParen = CloseAngle + 1;
    const int32 CloseParen = FIrTextUtils::FindMatchingChar(Expr, OpenParen, TEXT('('), TEXT(')'));
    if (CloseParen == INDEX_NONE)
    {
        return false;
    }

    FString Inner = Expr.Mid(OpenParen + 1, CloseParen - OpenParen - 1).TrimStartAndEnd();

    // Strip optional `Ident:` keyword prefix on the inner expression
    // (e.g. `cast<Actor>(Object: $X)` → inner is `$X`).
    int32 ColonIdx = INDEX_NONE;
    if (Inner.FindChar(TEXT(':'), ColonIdx))
    {
        const FString Prefix = Inner.Left(ColonIdx).TrimStartAndEnd();
        bool bIsIdent = !Prefix.IsEmpty();
        for (TCHAR Ch : Prefix)
        {
            if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
            {
                bIsIdent = false;
                break;
            }
        }
        if (bIsIdent)
        {
            Inner = Inner.Mid(ColonIdx + 1).TrimStartAndEnd();
        }
    }

    OutType = MoveTemp(TypeName);
    OutInner = MoveTemp(Inner);
    return true;
}

UEdGraphPin* FBpirValueResolver::ResolveCastExpression(const FString& ValueRef, FBpirEntryBlock& Block)
{
    FString TypeName;
    FString Inner;
    if (!ParseCastSyntax(ValueRef, TypeName, Inner))
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: '%s' is not a well-formed cast<T>(inner)"), *ValueRef);
        return nullptr;
    }

    if (Inner.IsEmpty())
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: '%s' has empty inner expression"), *ValueRef);
        return nullptr;
    }

    UEdGraphPin* InnerPin = ResolveValue(Inner, Block);
    if (!InnerPin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: failed to resolve inner expression '%s' of '%s'"), *Inner, *ValueRef);
        return nullptr;
    }

    UClass* TargetClass = ResolveUClass(TypeName);
    if (!TargetClass)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: could not resolve target class '%s' for '%s'"), *TypeName, *ValueRef);
        return nullptr;
    }

    // Pure casts have no exec flow — pass a throwaway exec slot.
    UEdGraphPin* DummyExec = nullptr;
    UK2Node_DynamicCast* CastNode = NodeEmitter.CreateCastNode(TargetClass, DummyExec);
    if (!CastNode)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: NodeEmitter failed to create cast node for '%s'"), *ValueRef);
        return nullptr;
    }

    // Strip the exec / "bSuccess" pins so the node represents a value-flow cast.
    CastNode->SetPurity(true);

    UEdGraphPin* SourcePin = CastNode->GetCastSourcePin();
    if (!SourcePin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: cast node for '%s' has no source pin"), *ValueRef);
        return nullptr;
    }

    const UEdGraphSchema* Schema = SourcePin->GetSchema();
    if (!Schema)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: cast source pin for '%s' has no schema"), *ValueRef);
        return nullptr;
    }

    if (!Schema->TryCreateConnection(InnerPin, SourcePin))
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: TryCreateConnection failed wiring inner pin into cast source for '%s'"), *ValueRef);
        return nullptr;
    }

    if (!SourcePin->LinkedTo.Contains(InnerPin))
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: TryCreateConnection did not wire inner pin into cast source for '%s'"), *ValueRef);
        return nullptr;
    }

    UEdGraphPin* ResultPin = CastNode->GetCastResultPin();
    if (!ResultPin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveCastExpression: cast node for '%s' has no result pin"), *ValueRef);
        return nullptr;
    }

    return ResultPin;
}

bool FBpirValueResolver::IsLiteral(const FString& ValueRef)
{
    if (ValueRef.IsEmpty())
    {
        return false;
    }

    // Quoted string
    if (ValueRef.StartsWith(TEXT("\"")))
    {
        return true;
    }

    // Boolean
    if (ValueRef == TEXT("true") || ValueRef == TEXT("false"))
    {
        return true;
    }

    // nullptr
    if (ValueRef == TEXT("nullptr"))
    {
        return true;
    }

    // Enum literal: contains ::
    if (ValueRef.Contains(TEXT("::")))
    {
        return true;
    }

    // Struct literal: starts with F and contains (
    if (ValueRef.Len() > 1 && ValueRef[0] == TEXT('F') && ValueRef.Contains(TEXT("(")))
    {
        return true;
    }

    // Unreal native struct literal: (Key=Value,...) format, e.g. (R=0.0,G=0.0,B=0.0,A=1.0)
    if (ValueRef.StartsWith(TEXT("(")) && ValueRef.EndsWith(TEXT(")")) && ValueRef.Contains(TEXT("=")))
    {
        return true;
    }

    // Asset path: starts with /Game/, /Engine/, or /Script/
    if (ValueRef.StartsWith(TEXT("/Game/")) || ValueRef.StartsWith(TEXT("/Engine/")) || ValueRef.StartsWith(TEXT("/Script/")))
    {
        return true;
    }

    // Numeric: integer or float, optionally negative or hex
    FString Work = ValueRef;

    // Strip optional leading minus
    if (Work.Len() > 0 && Work[0] == TEXT('-'))
    {
        Work = Work.Mid(1);
    }

    if (Work.IsEmpty())
    {
        return false;
    }

    // Hex integer: 0x...
    if (Work.StartsWith(TEXT("0x")) || Work.StartsWith(TEXT("0X")))
    {
        FString HexDigits = Work.Mid(2);
        if (HexDigits.IsEmpty())
        {
            return false;
        }
        for (TCHAR Ch : HexDigits)
        {
            if (!FChar::IsHexDigit(Ch))
            {
                return false;
            }
        }
        return true;
    }

    // Integer or float: digits, optional single dot, optional exponent
    bool bHasDot = false;
    bool bHasExponent = false;
    bool bHasDigit = false;

    for (int32 i = 0; i < Work.Len(); ++i)
    {
        TCHAR Ch = Work[i];

        if (FChar::IsDigit(Ch))
        {
            bHasDigit = true;
            continue;
        }

        if (Ch == TEXT('.') && !bHasDot && !bHasExponent)
        {
            bHasDot = true;
            continue;
        }

        if ((Ch == TEXT('e') || Ch == TEXT('E')) && !bHasExponent && bHasDigit)
        {
            bHasExponent = true;
            // Exponent can have optional +/- after it
            if (i + 1 < Work.Len() && (Work[i + 1] == TEXT('+') || Work[i + 1] == TEXT('-')))
            {
                ++i;
            }
            continue;
        }

        // Unreal float suffix 'f'
        if (Ch == TEXT('f') && i == Work.Len() - 1 && bHasDigit)
        {
            continue;
        }

        return false;
    }

    return bHasDigit;
}

FString FBpirValueResolver::GetLiteralText(const FString& ValueRef)
{
    if (ValueRef.IsEmpty())
    {
        return FString();
    }

    // Quoted string: strip surrounding quotes and reverse the escape applied by
    // BpirStructLiteralUtils::EscapeBpirString via UnescapeBpirString. Without this,
    // embedded quotes (e.g. NSLOCTEXT(...) emitted by the FText decompile path)
    // reach the value resolver / pin setter with literal backslashes, breaking
    // parses such as FTextStringHelper::CreateFromBuffer.
    if (ValueRef.StartsWith(TEXT("\"")) && ValueRef.EndsWith(TEXT("\"")) && ValueRef.Len() >= 2)
    {
        return BpirStructLiteralUtils::UnescapeBpirString(ValueRef.Mid(1, ValueRef.Len() - 2));
    }

    // nullptr -> None
    if (ValueRef == TEXT("nullptr"))
    {
        return TEXT("None");
    }

    // Positional struct literals (FVector / FRotator / FLinearColor / any
    // F<Ident>(...) form) are formatted reflectively via UScriptStruct::ExportText
    // so the result uses the canonical (X=,Y=,Z=) keyed form K2 pin defaults expect.
    {
        FString StructPinText;
        if (BpirStructLiteralUtils::TryFormatPositionalStructLiteralAsPinText(ValueRef, StructPinText))
        {
            return StructPinText;
        }
    }

    // Enum literal: EType::Value -> return as-is.
    // SetPinDefaultValue needs the full qualified name to find the UEnum and resolve
    // the value to a numeric index. Stripping the namespace breaks enum resolution.
    if (ValueRef.Contains(TEXT("::")))
    {
        return ValueRef;
    }

    // Numbers, bools, asset paths: return as-is
    return ValueRef;
}

UEnum* FBpirValueResolver::ResolveEnumTypeFromValueRef(const FString& ValueRef, const FBpirEntryBlock& Block)
{
    TSet<FString> VisitingRefs;
    return ResolveEnumTypeFromValueRefInternal(ValueRef, Block, VisitingRefs);
}

void FBpirValueResolver::InjectCachedVariable(const FString& VarName, UEdGraphPin* Pin)
{
    const FString NormalizedVarName = NormalizeBpirNameToken(VarName);

    if (!Pin || VariableGetCache.Contains(NormalizedVarName))
    {
        return;
    }

    VariableGetCache.Add(NormalizedVarName, Pin);
    PinResolver.RegisterVariable(NormalizedVarName, Pin);
}

void FBpirValueResolver::PreEmitExternalGet(const FString& TargetName, const FString& PropertyName)
{
    const FString NormalizedTargetName = NormalizeBpirNameToken(TargetName);
    TArray<FString> Segments = SplitNormalizedBpirPropertyPath(PropertyName);
    const FString NormalizedPropertyName = FString::Join(Segments, TEXT("."));

    // PropertyName may contain dots for chained access ($Payload.Transform.Location.X —
    // PreEmitVariableRefs splits only on the first dot, so `PropertyName` can still hold
    // "Transform.Location.X"). Full dotted form is also the ExternalGetCache key that
    // ResolveValue later looks up.
    FString CacheKey = NormalizedTargetName + TEXT(".") + NormalizedPropertyName;
    if (ExternalGetCache.Contains(CacheKey))
    {
        return;
    }

    // Resolve target to a pin (function parameter or self variable)
    UEdGraphPin* TargetPin = PinResolver.ResolveVariable(NormalizedTargetName);
    if (!TargetPin)
    {
        // Try the VariableGetCache (for $var that was pre-emitted)
        if (UEdGraphPin** CachedVarPin = VariableGetCache.Find(NormalizedTargetName))
        {
            TargetPin = *CachedVarPin;
        }
    }

    if (!TargetPin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitExternalGet: target '$%s' not found for property '%s'"), *NormalizedTargetName, *NormalizedPropertyName);
        return;
    }

    // Split the dotted PropertyName into segments. First segment is resolved here
    // (against the TargetPin type); remaining segments walk through the shared
    // ResolveChainFromPin helper.
    if (Segments.Num() == 0)
    {
        return;
    }

    const FString& FirstSegment = Segments[0];
    UEdGraphPin* FirstResultPin = nullptr;

    if (TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
    {
        FirstResultPin = ResolveStructMemberThroughPin(TargetPin, FirstSegment);
        if (!FirstResultPin)
        {
            return;
        }
    }
    else
    {
        // Extract UClass from the target pin type
        UClass* TargetClass = Cast<UClass>(TargetPin->PinType.PinSubCategoryObject.Get());
        if (!TargetClass)
        {
            UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitExternalGet: target '$%s' pin type is not an object class"), *NormalizedTargetName);
            return;
        }

        // Create an external VariableGet node for the first-segment property
        UK2Node_VariableGet* GetNode = NodeEmitter.CreateExternalVariableGetNode(FName(*FirstSegment), TargetClass);
        if (!GetNode)
        {
            UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitExternalGet: failed to create external get node for '%s.%s'"), *NormalizedTargetName, *FirstSegment);
            return;
        }

        // Wire the target pin
        UEdGraphPin* NodeTargetPin = GetNode->FindPin(UEdGraphSchema_K2::PN_Self);
        if (NodeTargetPin && TargetPin)
        {
            NodeTargetPin->MakeLinkTo(TargetPin);
            if (!NodeTargetPin->LinkedTo.Contains(TargetPin))
            {
                UE_LOG(LogBpirValueResolver, Warning, TEXT("PreEmitExternalGet: MakeLinkTo failed wiring target pin for '%s.%s'"),
                    *NormalizedTargetName, *FirstSegment);
            }
        }

        FirstResultPin = FindFirstDataOutputPin(GetNode);
        if (!FirstResultPin)
        {
            UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitExternalGet: external get node for '%s.%s' has no output pin"), *NormalizedTargetName, *FirstSegment);
            return;
        }
    }

    // If there are no remaining segments, the first-segment pin is the final result.
    if (Segments.Num() == 1)
    {
        ExternalGetCache.Add(CacheKey, FirstResultPin);
        return;
    }

    // Fold the remaining segments through the shared chain walker.
    TConstArrayView<FString> Remaining(Segments.GetData() + 1, Segments.Num() - 1);
    UEdGraphPin* FinalPin = ResolveChainFromPin(FirstResultPin, Remaining);
    if (FinalPin)
    {
        ExternalGetCache.Add(CacheKey, FinalPin);
    }
    else
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("PreEmitExternalGet: failed to resolve chain '%s.%s'"), *NormalizedTargetName, *NormalizedPropertyName);
    }
}

void FBpirValueResolver::SetGraph(UEdGraph* InGraph)
{
    Graph = InGraph;
    EmitMap = nullptr;
    VariableGetCache.Empty();
    MissingVariableGetCache.Empty();
    ExternalGetCache.Empty();
    AutoPropertyGetCache.Empty();
    AutoBreakStructCache.Empty();
    CachedSelfPin = nullptr;
}

UEdGraphPin* FBpirValueResolver::FindFirstDataOutputPin(UEdGraphNode* Node)
{
    if (!Node)
    {
        return nullptr;
    }
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            return Pin;
        }
    }
    return nullptr;
}

UEdGraphPin* FBpirValueResolver::FindOutputPinByName(UEdGraphNode* Node, const FString& PinName)
{
    if (!Node)
    {
        return nullptr;
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->Direction != EGPD_Output)
        {
            continue;
        }

        FString PinNameStr = Pin->PinName.ToString();
        if (PinNameStr.Equals(PinName, ESearchCase::IgnoreCase))
        {
            return Pin;
        }
    }

    // Fallback: match with spaces removed (e.g., "AsPhotoInspectionTrack" matches "As PhotoInspectionTrack")
    FString NormalizedPinName = PinName.Replace(TEXT(" "), TEXT(""));
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->Direction != EGPD_Output)
        {
            continue;
        }

        FString NormalizedNodePinName = Pin->PinName.ToString().Replace(TEXT(" "), TEXT(""));
        if (NormalizedNodePinName.Equals(NormalizedPinName, ESearchCase::IgnoreCase))
        {
            return Pin;
        }
    }

    // Fallback: BPIR accessor aliases (%handle.Item / .Result / .Value) map to the
    // sole non-exec output of array-getter / single-result K2Nodes whose real pin
    // name varies across UE versions (e.g. UK2Node_CallArrayFunction's "Output"
    // vs UK2Node_GetArrayItem's "Item"). Only applies when the exact-match and
    // space-normalized passes failed AND there is exactly one non-exec output.
    static const TCHAR* ArrayAccessorAliases[] = { TEXT("Item"), TEXT("Result"), TEXT("Value") };
    bool bIsAccessorAlias = false;
    for (const TCHAR* Alias : ArrayAccessorAliases)
    {
        if (PinName.Equals(Alias, ESearchCase::IgnoreCase))
        {
            bIsAccessorAlias = true;
            break;
        }
    }
    if (bIsAccessorAlias)
    {
        UEdGraphPin* Candidate = nullptr;
        int32 NonExecOutputCount = 0;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction != EGPD_Output)
            {
                continue;
            }
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }
            ++NonExecOutputCount;
            Candidate = Pin;
        }
        if (NonExecOutputCount == 1 && Candidate)
        {
            return Candidate;
        }
    }

    return nullptr;
}

UEdGraphPin* FBpirValueResolver::ResolveChainedPropertyAccess(UEdGraphNode* Node, const FString& FirstPinName, const TArray<FString>& PropertyChain)
{
    UEdGraphPin* CurrentPin = FindOutputPinByName(Node, FirstPinName);
    if (!CurrentPin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveChainedPropertyAccess: no output pin '%s' on node '%s'"),
            *FirstPinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        return nullptr;
    }

    return ResolveChainFromPin(CurrentPin, PropertyChain);
}

UEdGraphPin* FBpirValueResolver::ResolveChainFromPin(UEdGraphPin* StartPin, TConstArrayView<FString> RemainingChain)
{
    UEdGraphPin* CurrentPin = StartPin;
    if (!CurrentPin)
    {
        return nullptr;
    }

    for (const FString& PropertyName : RemainingChain)
    {
        const FName PinCategory = CurrentPin->PinType.PinCategory;

        if (PinCategory == UEdGraphSchema_K2::PC_Object || PinCategory == UEdGraphSchema_K2::PC_Interface)
        {
            // Object/Interface pin: create an ExternalVariableGet node for the property
            UClass* PinClass = Cast<UClass>(CurrentPin->PinType.PinSubCategoryObject.Get());
            if (!PinClass)
            {
                UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveChainFromPin: pin '%s' is object-typed but has no UClass"),
                    *CurrentPin->PinName.ToString());
                return nullptr;
            }

            // Build cache key for the VariableGet node
            FString VarCacheKey = FString::Printf(TEXT("%p_%s_%s"), CurrentPin, *CurrentPin->PinName.ToString(), *PropertyName);
            if (UEdGraphPin** CachedVarPin = AutoPropertyGetCache.Find(VarCacheKey))
            {
                CurrentPin = *CachedVarPin;
                continue;
            }

            UK2Node_VariableGet* GetNode = NodeEmitter.CreateExternalVariableGetNode(FName(*PropertyName), PinClass);
            if (!GetNode)
            {
                UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveChainFromPin: failed to create VariableGet for '%s' on class '%s'"),
                    *PropertyName, *PinClass->GetName());
                return nullptr;
            }

            // Wire the current pin to the self/target pin of the VariableGet node
            UEdGraphPin* SelfPin = GetNode->FindPin(UEdGraphSchema_K2::PN_Self);
            if (SelfPin)
            {
                SelfPin->MakeLinkTo(CurrentPin);
                if (!SelfPin->LinkedTo.Contains(CurrentPin))
                {
                    UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveChainFromPin: MakeLinkTo failed wiring self pin for property '%s'"),
                        *PropertyName);
                    return nullptr;
                }
            }

            UEdGraphPin* ValuePin = FindFirstDataOutputPin(GetNode);
            if (!ValuePin)
            {
                UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveChainFromPin: VariableGet for '%s' has no output pin"),
                    *PropertyName);
                return nullptr;
            }

            AutoPropertyGetCache.Add(VarCacheKey, ValuePin);
            CurrentPin = ValuePin;
        }
        else if (PinCategory == UEdGraphSchema_K2::PC_Struct)
        {
            UEdGraphPin* MemberPin = ResolveStructMemberThroughPin(CurrentPin, PropertyName);
            if (!MemberPin)
            {
                return nullptr;
            }
            CurrentPin = MemberPin;
        }
        else
        {
            UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveChainFromPin: pin '%s' (category '%s') is neither object nor struct — cannot chain property '%s'"),
                *CurrentPin->PinName.ToString(), *PinCategory.ToString(), *PropertyName);
            return nullptr;
        }
    }

    return CurrentPin;
}

UEdGraphPin* FBpirValueResolver::ResolveStructMemberThroughPin(UEdGraphPin* StructPin, const FString& MemberName)
{
    if (!StructPin || StructPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
    {
        return nullptr;
    }

    UScriptStruct* Struct = Cast<UScriptStruct>(StructPin->PinType.PinSubCategoryObject.Get());
    if (!Struct)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveStructMemberThroughPin: pin '%s' is struct-typed but has no UScriptStruct"),
            *StructPin->PinName.ToString());
        return nullptr;
    }

    UEdGraphNode* BreakOrCallNode = nullptr;
    if (UEdGraphNode** CachedNode = AutoBreakStructCache.Find(StructPin))
    {
        BreakOrCallNode = *CachedNode;
    }
    else
    {
        // Pick the break path off `HasNativeBreak` metadata.  Structs flagged
        // with HasNativeBreak (FRotator/FVector/FVector2D, FHitResult, FOverlapResult,
        // FTransform, etc.) MUST be broken via the helper UFunction the metadata
        // points at — UK2Node_BreakStruct deliberately refuses to expose pins for
        // them (CanBeBroken returns false on HasNativeBreak structs), and many
        // such structs (notably FHitResult) have *zero* CPF_BlueprintVisible
        // UPROPERTYs, so a fallback K2Node_BreakStruct emits the input pin and
        // no member pins at all.  The native break helper is the curated public
        // API and exposes every member as a typed output param.  Plain BlueprintType
        // structs without HasNativeBreak go through K2Node_BreakStruct as before.
        const bool bUseNativeBreak = Struct->HasMetaData(TEXT("HasNativeBreak"));

        if (!bUseNativeBreak)
        {
            UK2Node_BreakStruct* BreakNode = NodeEmitter.CreateBreakStructNode(Struct);
            if (!BreakNode)
            {
                UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveStructMemberThroughPin: failed to create BreakStruct for '%s'"),
                    *Struct->GetName());
                return nullptr;
            }

            for (UEdGraphPin* Pin : BreakNode->Pins)
            {
                if (Pin->Direction == EGPD_Input)
                {
                    Pin->MakeLinkTo(StructPin);
                    if (!Pin->LinkedTo.Contains(StructPin))
                    {
                        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveStructMemberThroughPin: MakeLinkTo failed wiring BreakStruct input for '%s'"),
                            *Struct->GetName());
                        return nullptr;
                    }
                    break;
                }
            }

            BreakOrCallNode = BreakNode;
        }
        else
        {
            FString NativeBreakPath = Struct->GetMetaData(TEXT("HasNativeBreak"));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            UFunction* NativeFunc = FindObject<UFunction>(nullptr, *NativeBreakPath, EFindObjectFlags::ExactClass);
#else
            UFunction* NativeFunc = FindObject<UFunction>(nullptr, *NativeBreakPath, true);
#endif
            if (!NativeFunc)
            {
                UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveStructMemberThroughPin: HasNativeBreak metadata on '%s' points to unresolved function '%s'"),
                    *Struct->GetName(), *NativeBreakPath);
                return nullptr;
            }

            UEdGraphPin* UnusedExecPin = nullptr;
            UK2Node_CallFunction* CallNode = NodeEmitter.CreateCallFunctionNode(NativeFunc, UnusedExecPin);
            if (!CallNode)
            {
                UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveStructMemberThroughPin: failed to create CallFunction node for native break of '%s'"),
                    *Struct->GetName());
                return nullptr;
            }

            for (UEdGraphPin* Pin : CallNode->Pins)
            {
                if (Pin->Direction == EGPD_Input
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                    && Pin->PinName != UEdGraphSchema_K2::PN_Self)
                {
                    Pin->MakeLinkTo(StructPin);
                    if (!Pin->LinkedTo.Contains(StructPin))
                    {
                        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveStructMemberThroughPin: MakeLinkTo failed wiring CallFunction input for native break of '%s'"),
                            *Struct->GetName());
                        return nullptr;
                    }
                    break;
                }
            }

            BreakOrCallNode = CallNode;
        }

        AutoBreakStructCache.Add(StructPin, BreakOrCallNode);
    }

    UEdGraphPin* MemberPin = FindOutputPinByName(BreakOrCallNode, MemberName);
    if (!MemberPin)
    {
        UE_LOG(LogBpirValueResolver, Error, TEXT("ResolveStructMemberThroughPin: BreakStruct for '%s' has no member pin '%s'"),
            *Struct->GetName(), *MemberName);
    }

    return MemberPin;
}
