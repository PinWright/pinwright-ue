// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirCompiler.cpp - Three-pass BPIR compiler: parse -> emit -> wire

#include "Compiler/BpirCompiler.h"
#include "State/PluginState.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirValueResolver.h"
#include "Compiler/CodeNodeEmitter.h"
#include "Compiler/CodePinResolver.h"
#include "Compiler/CodeFunctionResolver.h"
#include "Compiler/BpirSharedConstants.h"
#include "Compiler/BpirShapeMetadata.h"
#include "Compiler/NodeLayoutEngine.h"
#include "Decompiler/BpirInputKeyHelpers.h"
#include "IrCore/IrTextUtils.h"
#include "Decompiler/AnimGraphFamilyCheck.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintEnumHelpers.h"
#include "Utils/ClassUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/PropertyImport.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_InputKey.h"
#include "K2Node_Tunnel.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Select.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Self.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_FormatText.h"
#include "K2Node_AsyncAction.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "UObject/UObjectIterator.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node.h"
#if __has_include("K2Node_CallDelegate.h")
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CreateDelegate.h"
#define BPIR_HAS_DELEGATE_NODES 1
#else
#define BPIR_HAS_DELEGATE_NODES 0
#endif
#include "EdGraphNode_Comment.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "GameFramework/Actor.h"
#include "InputAction.h"
#include "Compat/EngineVersionCompat.h"

DEFINE_LOG_CATEGORY(LogBpirCompiler);

// ----------------------------------------------------------------------------
// Static helpers
// ----------------------------------------------------------------------------

static FString NormalizeBpirNameToken(const FString& Text)
{
    FString Name;
    FString Error;
    if (FIrTextUtils::TryUnwrapNameToken(Text, Name, Error))
    {
        return Name;
    }
    return Text.TrimStartAndEnd();
}

static TArray<FString> SplitNormalizedBpirPropertyPath(const FString& PropertyPath)
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

static FString NormalizeBpirPropertyPath(const FString& PropertyPath)
{
    return FString::Join(SplitNormalizedBpirPropertyPath(PropertyPath), TEXT("."));
}

static bool SplitDollarReference(const FString& Reference, FString& OutTargetName, FString& OutPropertyName)
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

static bool NodeHasExecPins(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return false;
    }

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return true;
        }
    }
    return false;
}

// Compact opcode -> string for structured wire-up diagnostics.
// Kept file-local so we don't widen the IR header just for log strings.
static const TCHAR* BpirOpcodeName(EBpirOpcode Opcode)
{
    switch (Opcode)
    {
    case EBpirOpcode::Call:                   return TEXT("Call");
    case EBpirOpcode::Pure:                   return TEXT("Pure");
    case EBpirOpcode::Latent:                 return TEXT("Latent");
    case EBpirOpcode::Set:                    return TEXT("Set");
    case EBpirOpcode::Get:                    return TEXT("Get");
    case EBpirOpcode::Branch:                 return TEXT("Branch");
    case EBpirOpcode::Foreach:                return TEXT("Foreach");
    case EBpirOpcode::ForeachBreak:           return TEXT("ForeachBreak");
    case EBpirOpcode::While:                  return TEXT("While");
    case EBpirOpcode::Switch:                 return TEXT("Switch");
    case EBpirOpcode::Sequence:               return TEXT("Sequence");
    case EBpirOpcode::Cast:                   return TEXT("Cast");
    case EBpirOpcode::Select:                 return TEXT("Select");
    case EBpirOpcode::Macro:                  return TEXT("Macro");
    case EBpirOpcode::Timeline:               return TEXT("Timeline");
    case EBpirOpcode::BreakStruct:            return TEXT("BreakStruct");
    case EBpirOpcode::MakeStruct:             return TEXT("MakeStruct");
    case EBpirOpcode::Self:                   return TEXT("Self");
    case EBpirOpcode::Enum:                   return TEXT("Enum");
    case EBpirOpcode::MakeArray:              return TEXT("MakeArray");
    case EBpirOpcode::Subsystem:              return TEXT("Subsystem");
    case EBpirOpcode::Return:                 return TEXT("Return");
    case EBpirOpcode::End:                    return TEXT("End");
    case EBpirOpcode::ExecGoto:               return TEXT("ExecGoto");
    case EBpirOpcode::Label:                  return TEXT("Label");
    case EBpirOpcode::Comment:                return TEXT("Comment");
    case EBpirOpcode::CallDispatcher:         return TEXT("CallDispatcher");
    case EBpirOpcode::BindDispatcher:         return TEXT("BindDispatcher");
    case EBpirOpcode::UnbindDispatcher:       return TEXT("UnbindDispatcher");
    case EBpirOpcode::SwitchInt:              return TEXT("SwitchInt");
    case EBpirOpcode::SwitchString:           return TEXT("SwitchString");
    case EBpirOpcode::SwitchEnum:             return TEXT("SwitchEnum");
    case EBpirOpcode::ClearDispatcher:        return TEXT("ClearDispatcher");
    case EBpirOpcode::FieldNotifySubscribe:   return TEXT("FieldNotifySubscribe");
    case EBpirOpcode::FieldNotifyUnsubscribe: return TEXT("FieldNotifyUnsubscribe");
    case EBpirOpcode::Alias:                  return TEXT("Alias");
    }
    return TEXT("<unknown>");
}

static bool IsSelectIndexArgName(const FString& PinName)
{
    const FString LowerName = NormalizeBpirNameToken(PinName).ToLower();
    return LowerName == TEXT("cond")
        || LowerName == TEXT("condition")
        || LowerName == TEXT("index");
}

static UEnum* ResolveEnumFromPinType(const FEdGraphPinType& PinType)
{
    if (PinType.PinCategory != UEdGraphSchema_K2::PC_Byte
        && PinType.PinCategory != UEdGraphSchema_K2::PC_Enum)
    {
        return nullptr;
    }
    return Cast<UEnum>(PinType.PinSubCategoryObject.Get());
}

static UEnum* ResolveSelectIndexEnum(
    const FBpirInstruction& Inst,
    FBpirEntryBlock& Block,
    FBpirValueResolver* ValueResolver)
{
    for (const FBpirArg& Arg : Inst.Args)
    {
        if (!IsSelectIndexArgName(Arg.PinName))
        {
            continue;
        }

        if (ValueResolver
            && !FBpirValueResolver::IsLiteral(Arg.Value))
        {
            if (UEdGraphPin* SourcePin = ValueResolver->ResolveValue(Arg.Value, Block))
            {
                if (UEnum* EnumType = ResolveEnumFromPinType(SourcePin->PinType))
                {
                    return EnumType;
                }
            }
        }

        if (UEnum* EnumType = FBpirValueResolver::ResolveEnumTypeFromValueRef(Arg.Value, Block))
        {
            return EnumType;
        }

        break;
    }

    return nullptr;
}

static void BuildSelectOptionPinsByEnumValue(UK2Node_Select* SelectNode, TMap<int64, UEdGraphPin*>& OutPinsByValue)
{
    OutPinsByValue.Reset();
    if (!SelectNode)
    {
        return;
    }

    if (const UEnum* EnumType = SelectNode->GetEnum())
    {
        for (UEdGraphPin* Pin : SelectNode->Pins)
        {
            if (!Pin
                || Pin->Direction != EGPD_Input
                || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                || Pin->PinName == UEdGraphSchema_K2::PN_Self
                || Pin->PinName == FName(TEXT("Index")))
            {
                continue;
            }

            int64 PinValue = INDEX_NONE;
            if (BlueprintHandlerUtils::TryResolveEnumLiteralToValue(EnumType, Pin->PinName.ToString(), PinValue))
            {
                OutPinsByValue.Add(PinValue, Pin);
            }
        }
    }
}

static UEdGraphPin* FindSelectOptionPinByLabel(
    UK2Node_Select* SelectNode,
    const FString& Label,
    const TMap<int64, UEdGraphPin*>& OptionPinsByValue)
{
    if (!SelectNode)
    {
        return nullptr;
    }

    const FString CleanLabel = NormalizeBpirNameToken(Label);
    if (const UEnum* EnumType = SelectNode->GetEnum())
    {
        int64 RequestedValue = INDEX_NONE;
        if (BlueprintHandlerUtils::TryResolveEnumLiteralToValue(EnumType, CleanLabel, RequestedValue))
        {
            UEdGraphPin* const* FoundPin = OptionPinsByValue.Find(RequestedValue);
            return FoundPin ? *FoundPin : nullptr;
        }
    }

    return SelectNode->FindPin(*CleanLabel, EGPD_Input);
}

// Decide the pin type a Select's Return Value and option pins should carry
// before any option literal is applied.
//
// UK2Node_Select allocates every option pin and the Return Value as
// PC_Wildcard and only types them once something connects. Option literals are
// applied at the Select's own instruction index, while the Return Value is
// typed only when a later instruction consumes %ref — so the literal always
// lands on a wildcard pin, where FCodePinResolver::SetPinDefaultValue can only
// write the raw string into DefaultValue. That is the slot a PC_Text pin never
// reads (UK2Node_Select::ExpandNode copies DefaultTextValue into the literal
// term), so a localized FText option compiles to empty text.
//
// Two sources are consulted, most explicit first:
//   1. The `%name: Type =` register-binding annotation, which the decompiler
//      already emits for every Select (BpirTextEmitter's
//      ResolvePrimaryOutputTypeAnnotation), so a decompile/recompile round-trip
//      restores the type it observed.
//   2. An option literal that parses as an FText carrying a real localization
//      identity (NSLOCTEXT / LOCTABLE forms). A bare quoted string has no
//      identity and stays a PC_String literal, so this cannot mistype an
//      ordinary string select.
// Returns false when neither source is definitive; the pins then stay wildcard
// and behave exactly as before.
static bool ResolveSelectResultPinType(const FBpirInstruction& Inst, FEdGraphPinType& OutType)
{
    if (Inst.bHasDeclaredResultType
        && FCodePinResolver::ConvertTypeSpecToPinType(Inst.DeclaredResultType, OutType))
    {
        return true;
    }

    for (const FBpirArg& Arg : Inst.Args)
    {
        if (IsSelectIndexArgName(Arg.PinName) || !FBpirValueResolver::IsLiteral(Arg.Value))
        {
            continue;
        }

        FText ParsedText;
        FString TextError;
        if (CoerceStringToPersistedFText(
                FBpirValueResolver::GetLiteralText(Arg.Value), nullptr, ParsedText, TextError))
        {
            OutType = FEdGraphPinType();
            OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
            return true;
        }
    }

    return false;
}

// Stamp PinType onto the Select's Return Value and every still-wildcard option
// pin. This mirrors the non-index branch of UK2Node_Select::OnPinTypeChanged
// without its bReconstructNode side effect: at emit time the pins are freshly
// allocated wildcards with no split sub-pins, no links and no defaults to
// invalidate, so the assignment is the whole of the work and no reconstruction
// is owed. Skipping the reconstruct also keeps every pin pointer the emit pass
// just cached valid for the wiring pass.
//
// Option pins are found by walking Pins rather than via GetOptionPins(): that
// accessor keys off IndexPinType, which UK2Node_Select::SetEnum leaves at its
// PC_Wildcard default, so on a freshly emitted enum-backed Select it matches
// the "Option N" naming and returns nothing.
static void PreTypeSelectPins(UK2Node_Select* SelectNode, const FEdGraphPinType& PinType)
{
    if (!SelectNode)
    {
        return;
    }

    UEdGraphPin* ReturnPin = SelectNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    if (!ReturnPin || ReturnPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
    {
        return;
    }
    ReturnPin->PinType = PinType;

    for (UEdGraphPin* Pin : SelectNode->Pins)
    {
        if (Pin
            && Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
            && Pin->PinName != UEdGraphSchema_K2::PN_Self
            && Pin->PinName != FName(TEXT("Index")))
        {
            Pin->PinType = PinType;
        }
    }
}

// Decide the pin type a MakeArray's output pin should carry before any element
// literal is applied. The element pins take the same type with the container
// stripped.
//
// UK2Node_MakeContainer::AllocateDefaultPins allocates the output pin and every
// `[N]` element pin as PC_Wildcard, and AddInputPin copies the (still wildcard)
// output type onto each pin the emitter adds. Element literals are applied at
// the MakeArray's own instruction index, while the output pin is typed only
// when a later instruction consumes %ref and the engine's
// NotifyPinConnectionListChanged propagates the linked type back down — so the
// literal always lands on a wildcard pin, where
// FCodePinResolver::SetPinDefaultValue can only write the raw string into
// DefaultValue. That is the slot a PC_Text pin never reads, so a localized
// FText element compiles to empty text. Same defect, same shape, as
// B-bpir-select-literal-text-lost.
//
// Two sources are consulted, most explicit first:
//   1. The `%name: array<T> =` register-binding annotation. BpirParser reads it
//      for every named-result instruction, and BpirTextEmitter's MakeArray
//      branch emits it from the output pin (ResolvePrimaryOutputTypeAnnotation),
//      so a decompile/recompile round-trip restores the type it observed. A
//      wildcard source node annotates as `array<wildcard>`, which is skipped
//      outright, and an annotation that is not an array is not this node's
//      result type — both fall through.
//   2. An element literal that parses as an FText carrying a real localization
//      identity (NSLOCTEXT / LOCTABLE forms). A bare quoted string has no
//      identity and stays a PC_String literal, so this cannot mistype an
//      ordinary string array.
// Returns false when neither source is definitive; the pins then stay wildcard
// and behave exactly as before.
static bool ResolveMakeArrayOutputPinType(const FBpirInstruction& Inst, FEdGraphPinType& OutType)
{
    // `array<wildcard>` is what PinTypeToBpirType emits for an untyped MakeArray
    // (PC_Wildcard has no grammar entry, so the raw pin category is printed).
    // It is not a type — converting it would only log an unresolved-identifier
    // warning on every round-trip of a wildcard array.
    const bool bDeclaredElementIsWildcard =
        Inst.bHasDeclaredResultType
        && Inst.DeclaredResultType.ElementSpec.IsValid()
        && Inst.DeclaredResultType.ElementSpec->Kind == EBpirTypeKind::Unresolved
        && Inst.DeclaredResultType.ElementSpec->InnerName == UEdGraphSchema_K2::PC_Wildcard;

    FEdGraphPinType DeclaredType;
    if (Inst.bHasDeclaredResultType
        && !bDeclaredElementIsWildcard
        && FCodePinResolver::ConvertTypeSpecToPinType(Inst.DeclaredResultType, DeclaredType)
        && DeclaredType.IsArray()
        && DeclaredType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
    {
        OutType = MoveTemp(DeclaredType);
        return true;
    }

    for (const FBpirArg& Arg : Inst.Args)
    {
        if (!FBpirValueResolver::IsLiteral(Arg.Value))
        {
            continue;
        }

        FText ParsedText;
        FString TextError;
        if (CoerceStringToPersistedFText(
                FBpirValueResolver::GetLiteralText(Arg.Value), nullptr, ParsedText, TextError))
        {
            OutType = FEdGraphPinType();
            OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
            OutType.ContainerType = EPinContainerType::Array;
            return true;
        }
    }

    return false;
}

// Stamp ArrayPinType onto the MakeArray's output pin and the element type onto
// every still-wildcard element pin. This mirrors what
// UK2Node_MakeContainer::PropagatePinType does for the pins that matter here,
// without calling it: it is protected, it re-runs
// SetPinAutogeneratedDefaultValueBasedOnType over every pin, and it fires
// NotifyNodeChanged twice. At emit time the pins are freshly allocated
// wildcards with no split sub-pins, no links and no defaults to invalidate, so
// the assignment is the whole of the work.
//
// Typing the OUTPUT pin is load-bearing, not cosmetic: NotifyPinConnectionListChanged
// only adopts a linked type while the output pin is still PC_Wildcard, so
// stamping it keeps the engine from re-propagating (and re-defaulting) the
// element pins when a later instruction wires %ref away.
//
// Element pins are found by walking Pins rather than via GetKeyAndValuePins():
// that accessor returns every top-level input pin in its *KeyPins* out-param
// for a non-map container, which reads as a bug at the call site.
static void PreTypeMakeArrayPins(UK2Node_MakeArray* ArrayNode, const FEdGraphPinType& ArrayPinType)
{
    if (!ArrayNode)
    {
        return;
    }

    UEdGraphPin* OutputPin = ArrayNode->GetOutputPin();
    if (!OutputPin || OutputPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
    {
        return;
    }
    OutputPin->PinType = ArrayPinType;

    FEdGraphPinType ElementType = ArrayPinType;
    ElementType.ContainerType = EPinContainerType::None;

    for (UEdGraphPin* Pin : ArrayNode->Pins)
    {
        if (Pin
            && Pin->Direction == EGPD_Input
            && Pin->ParentPin == nullptr
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
        {
            Pin->PinType = ElementType;
        }
    }
}

// Read-only context handed to each per-opcode pin-resolution strategy. The
// strategies need none of the compiler's mutable member state — only the
// already-emitted node, the instruction, and the Select-with-enum lookup
// table (non-null/populated only for the Select opcode).
struct FWireDataPinsContext
{
    const FEmittedNodeInfo& Emit;
    const FBpirInstruction& Inst;
    UK2Node_Select* EnumSelectNode;
    const TMap<int64, UEdGraphPin*>& SelectOptionPinsByEnumValue;
};

// One strategy per opcode: maps (ArgIdx, Arg) to the target UEdGraphPin* on the
// emitted node, or nullptr if no pin matches (caller raises the missing-pin
// diagnostic). Each body is a verbatim move of the corresponding switch case.
static UEdGraphPin* ResolveTargetPin_Branch(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (ArgIdx == 0)
    {
        return Ctx.Emit.Node->FindPin(TEXT("Condition"));
    }
    return nullptr;
}

static UEdGraphPin* ResolveTargetPin_Foreach(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (ArgIdx == 0)
    {
        return Ctx.Emit.Node->FindPin(TEXT("Array"));
    }
    return Ctx.Emit.Node->FindPin(*Arg.PinName);
}

static UEdGraphPin* ResolveTargetPin_While(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (ArgIdx == 0)
    {
        return Ctx.Emit.Node->FindPin(TEXT("Condition"));
    }
    return Ctx.Emit.Node->FindPin(*Arg.PinName);
}

static UEdGraphPin* ResolveTargetPin_Switch(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (ArgIdx == 0)
    {
        return Ctx.Emit.Node->FindPin(TEXT("Selection"));
    }
    return Ctx.Emit.Node->FindPin(*Arg.PinName);
}

static UEdGraphPin* ResolveTargetPin_Cast(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (ArgIdx == 0)
    {
        UEdGraphPin* TargetPin = Ctx.Emit.Node->FindPin(TEXT("Object"));
        // Fallback: find the first non-exec input pin
        if (!TargetPin)
        {
            for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
            {
                if (Pin->Direction == EGPD_Input
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    return Pin;
                }
            }
        }
        return TargetPin;
    }
    return nullptr;
}

static UEdGraphPin* ResolveTargetPin_Set(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (ArgIdx == 0)
    {
        // Find the variable's input pin (first non-exec, non-self input)
        for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
        {
            if (Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                && Pin->PinName != UEdGraphSchema_K2::PN_Self)
            {
                return Pin;
            }
        }
    }
    return nullptr;
}

static UEdGraphPin* ResolveTargetPin_Return(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (!Arg.PinName.IsEmpty() && Arg.PinName != TEXT("ReturnValue"))
    {
        // Named return arg (macro style): find pin by name
        return Ctx.Emit.Node->FindPin(*Arg.PinName, EGPD_Input);
    }
    if (ArgIdx == 0)
    {
        // Positional (function style): wire to first non-exec input pin
        for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
        {
            if (Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                return Pin;
            }
        }
        return nullptr;
    }
    return Ctx.Emit.Node->FindPin(*Arg.PinName);
}

static UEdGraphPin* ResolveTargetPin_BreakStruct(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    if (ArgIdx == 0)
    {
        // Wire to the struct input pin (first non-exec, non-self input).
        // When routed to a CallFunction node (native break), exclude PN_Self.
        for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
        {
            if (Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                && Pin->PinName != UEdGraphSchema_K2::PN_Self)
            {
                return Pin;
            }
        }
    }
    return nullptr;
}

static UEdGraphPin* ResolveTargetPin_MakeArray(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    // Positional arguments: wire to [0], [1], etc.
    FString IndexPinName = FString::Printf(TEXT("[%d]"), ArgIdx);
    return Ctx.Emit.Node->FindPin(*IndexPinName);
}

static UEdGraphPin* ResolveTargetPin_Select(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    FString LowerName = Arg.PinName.ToLower();
    if (IsSelectIndexArgName(Arg.PinName))
    {
        UEdGraphPin* TargetPin = Ctx.Emit.Node->FindPin(TEXT("Index"));
        if (!TargetPin)
        {
            for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
            {
                if (Pin->Direction == EGPD_Input
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
                {
                    return Pin;
                }
            }
        }
        return TargetPin;
    }
    if (Ctx.EnumSelectNode)
    {
        return FindSelectOptionPinByLabel(Ctx.EnumSelectNode, Arg.PinName, Ctx.SelectOptionPinsByEnumValue);
    }
    if (LowerName == TEXT("true"))
    {
        // UK2Node_Select gives Option 1 the friendly name "True"
        // (Idx == 0 ? CoreTexts.False : CoreTexts.True in
        // AllocateDefaultPins), so the true branch wires to Option 1.
        return Ctx.Emit.Node->FindPin(TEXT("Option 1"));
    }
    if (LowerName == TEXT("false"))
    {
        return Ctx.Emit.Node->FindPin(TEXT("Option 0"));
    }

    UEdGraphPin* TargetPin = Ctx.Emit.Node->FindPin(*Arg.PinName);
    // Fallback: normalized name matching (e.g. "Option2" -> "Option 2")
    if (!TargetPin)
    {
        FString NormalizedArgName = Arg.PinName.Replace(TEXT(" "), TEXT(""));
        for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
        {
            if (Pin->Direction == EGPD_Input)
            {
                FString NormalizedPinName = Pin->PinName.ToString().Replace(TEXT(" "), TEXT(""));
                if (NormalizedPinName.Equals(NormalizedArgName, ESearchCase::IgnoreCase))
                {
                    return Pin;
                }
            }
        }
    }
    return TargetPin;
}

static UEdGraphPin* ResolveTargetPin_Default(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx)
{
    // Generic: match by pin name
    if (!Arg.PinName.IsEmpty())
    {
        UEdGraphPin* TargetPin = Ctx.Emit.Node->FindPin(*Arg.PinName);

        // Fallback: case-insensitive search and normalized name matching
        if (!TargetPin)
        {
            FString NormalizedArgName = Arg.PinName.Replace(TEXT(" "), TEXT(""));
            for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
            {
                if (Pin->Direction == EGPD_Input)
                {
                    if (Pin->PinName.ToString().Equals(Arg.PinName, ESearchCase::IgnoreCase))
                    {
                        return Pin;
                    }
                    FString NormalizedPinName = Pin->PinName.ToString().Replace(TEXT(" "), TEXT(""));
                    if (NormalizedPinName.Equals(NormalizedArgName, ESearchCase::IgnoreCase))
                    {
                        return Pin;
                    }
                }
            }
        }

        // Target -> self alias: UK2Node_CallFunction names the object input "self",
        // but BPIR convention uses "Target"
        if (!TargetPin && Arg.PinName.Equals(TEXT("Target"), ESearchCase::IgnoreCase))
        {
            return Ctx.Emit.Node->FindPin(UEdGraphSchema_K2::PN_Self);
        }
        return TargetPin;
    }

    if (Ctx.Inst.Opcode == EBpirOpcode::Call
        || Ctx.Inst.Opcode == EBpirOpcode::Pure
        || Ctx.Inst.Opcode == EBpirOpcode::Latent)
    {
        // Positional argument: find the Nth non-exec input pin (counting only
        // positional args seen so far, not named ones)
        int32 PositionalIdx = 0;
        for (int32 PrevIdx = 0; PrevIdx < ArgIdx; ++PrevIdx)
        {
            if (Ctx.Inst.Args[PrevIdx].PinName.IsEmpty())
            {
                ++PositionalIdx;
            }
        }

        int32 DataInputIdx = 0;
        for (UEdGraphPin* Pin : Ctx.Emit.Node->Pins)
        {
            if (Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                if (DataInputIdx == PositionalIdx)
                {
                    return Pin;
                }
                ++DataInputIdx;
            }
        }
    }
    return nullptr;
}

// Dispatches to the per-opcode strategy. Timeline track args are handled during
// the emit phase, not pin wiring, so the dispatcher sets bSkipArg to reproduce
// the original switch's `continue` for that opcode.
static UEdGraphPin* ResolveTargetPinForOpcode(int32 ArgIdx, const FBpirArg& Arg, const FWireDataPinsContext& Ctx, bool& bSkipArg)
{
    bSkipArg = false;
    switch (Ctx.Inst.Opcode)
    {
    case EBpirOpcode::Branch:        return ResolveTargetPin_Branch(ArgIdx, Arg, Ctx);
    case EBpirOpcode::Foreach:
    case EBpirOpcode::ForeachBreak:  return ResolveTargetPin_Foreach(ArgIdx, Arg, Ctx);
    case EBpirOpcode::While:         return ResolveTargetPin_While(ArgIdx, Arg, Ctx);
    case EBpirOpcode::Switch:
    case EBpirOpcode::SwitchInt:
    case EBpirOpcode::SwitchString:
    case EBpirOpcode::SwitchEnum:    return ResolveTargetPin_Switch(ArgIdx, Arg, Ctx);
    case EBpirOpcode::Cast:          return ResolveTargetPin_Cast(ArgIdx, Arg, Ctx);
    case EBpirOpcode::Set:           return ResolveTargetPin_Set(ArgIdx, Arg, Ctx);
    case EBpirOpcode::Return:        return ResolveTargetPin_Return(ArgIdx, Arg, Ctx);
    case EBpirOpcode::BreakStruct:   return ResolveTargetPin_BreakStruct(ArgIdx, Arg, Ctx);
    case EBpirOpcode::MakeArray:     return ResolveTargetPin_MakeArray(ArgIdx, Arg, Ctx);
    case EBpirOpcode::Select:        return ResolveTargetPin_Select(ArgIdx, Arg, Ctx);
    case EBpirOpcode::Timeline:
        // Track args handled during emit phase, not pin wiring
        bSkipArg = true;
        return nullptr;
    default:                         return ResolveTargetPin_Default(ArgIdx, Arg, Ctx);
    }
}

struct FBpirMacroCallerLinkSnapshot
{
    TWeakObjectPtr<UEdGraphNode> LinkedNode;
    FName LinkedPinName;
    EEdGraphPinDirection LinkedDirection = EGPD_MAX;
};

struct FBpirMacroCallerPinSnapshot
{
    FName PinName;
    EEdGraphPinDirection Direction = EGPD_MAX;
    FEdGraphPinType PinType;
    FString DefaultValue;
    FText DefaultTextValue;
    TWeakObjectPtr<UObject> DefaultObject;
    TArray<FBpirMacroCallerLinkSnapshot> Links;
};

struct FBpirMacroCallerSnapshot
{
    TWeakObjectPtr<UK2Node_MacroInstance> MacroInstance;
    FString MacroName;
    TArray<FBpirMacroCallerPinSnapshot> Pins;
};

static bool ReconstructSameBlueprintMacroCallers(
    UBlueprint* Blueprint,
    const TSet<UEdGraph*>& MacroGraphs,
    const TSet<FGuid>& CreatedNodeGuids,
    TArray<FCompileError>& OutErrors,
    TArray<FBpirMacroCallerSnapshot>& OutCallerSnapshots);

// Looks up the exec output pin for one enum entry on a SwitchEnum node.
// Tries EntryName as-is first, then the StripEnumScope form (UE pins often drop the
// type prefix, e.g. "ESlateVisibility::Visible" → "Visible").
static UEdGraphPin* FindSwitchEnumEntryPin(UK2Node_SwitchEnum* SwitchNode, const FString& EntryName)
{
    if (!SwitchNode)
    {
        return nullptr;
    }
    if (UEdGraphPin* Pin = SwitchNode->FindPin(*EntryName))
    {
        return Pin;
    }
    const FString Unscoped = BlueprintHandlerUtils::StripEnumScope(EntryName);
    if (!Unscoped.Equals(EntryName, ESearchCase::CaseSensitive))
    {
        return SwitchNode->FindPin(*Unscoped);
    }
    return nullptr;
}

// Parses one `switch_int` case label into its integer value.
//
// The label doubles as the exec pin's name and, through
// UK2Node_Switch::GetExportTextForPin, as the literal the Blueprint compiler
// compares the selection against. The engine writes those names back with
// FString::Printf(TEXT("%d")), so any spelling that does not survive a
// FromInt round trip ("01", "+1", "1.0") would be silently rewritten to a
// different-looking label on the first reconstruction. Reject it here instead.
static bool TryParseSwitchIntCaseLabel(const FString& Label, int32& OutValue)
{
    if (!Label.IsNumeric())
    {
        return false;
    }
    OutValue = FCString::Atoi(*Label);
    return FString::FromInt(OutValue).Equals(Label, ESearchCase::CaseSensitive);
}

// Resolves a `switch_int` instruction's case labels to the exec pin values the
// node must carry, sorted ascending, or reports why the label set cannot be
// represented.
//
// UK2Node_SwitchInteger persists no per-case value: the case pin's *name* is the
// case value, and nothing records which arm was authored for which label. So
// UK2Node_SwitchInteger::ReallocatePinsDuringReconstruction rebuilds the labels
// positionally — it walks the exec output pins in pin order and renames each one
// to StartIndex, StartIndex + 1, ... (GetPinNameGivenIndex does not add
// StartIndex; the counter is seeded with it). That runs on compile-on-load
// (FBlueprintEditorUtils::ReconstructAllNodes under bIsRegeneratingOnLoad), on a
// StartIndex / bHasDefaultPin property change, and on a manual node refresh — but
// not on the in-editor compile that authored the node, which is why a decompile
// taken straight after compiling looks clean.
//
// The only label set that survives that renumber is a contiguous ascending run
// beginning at StartIndex, laid out in ascending pin order. Callers therefore
// anchor StartIndex to the lowest label and create the pins in the order returned
// here. A gapped or duplicated set has no representation at all and is rejected:
// compiling it would produce a node that dispatches to a different arm after the
// next load, with nothing logged at any point.
static bool ResolveSwitchIntCaseValues(
    const TArray<FBpirExecTarget>& ExecTargets,
    TArray<int32>& OutSortedValues,
    FString& OutError)
{
    OutSortedValues.Reset();

    for (const FBpirExecTarget& Target : ExecTargets)
    {
        if (Target.PinName.Equals(TEXT("default"), ESearchCase::IgnoreCase))
        {
            continue;
        }

        int32 Value = 0;
        if (!TryParseSwitchIntCaseLabel(Target.PinName, Value))
        {
            OutError = FString::Printf(
                TEXT("switch_int case label '%s' is not a plain decimal integer. ")
                TEXT("Write case labels as bare integers (0, 1, -2) — the label becomes the ")
                TEXT("case pin's name, and the engine rewrites that name in canonical decimal ")
                TEXT("form on the next reconstruction."),
                *Target.PinName);
            return false;
        }
        OutSortedValues.Add(Value);
    }

    OutSortedValues.Sort();

    for (int32 Index = 1; Index < OutSortedValues.Num(); ++Index)
    {
        if (OutSortedValues[Index] == OutSortedValues[Index - 1])
        {
            OutError = FString::Printf(
                TEXT("switch_int case label %d is used twice. Each case label must be distinct."),
                OutSortedValues[Index]);
            return false;
        }
        if (OutSortedValues[Index] != OutSortedValues[Index - 1] + 1)
        {
            FString Listed;
            for (const int32 Value : OutSortedValues)
            {
                Listed += Listed.IsEmpty() ? FString::FromInt(Value)
                                           : FString::Printf(TEXT(", %d"), Value);
            }
            OutError = FString::Printf(
                TEXT("switch_int case labels must form a contiguous ascending run (e.g. [3, 4, 5]); ")
                TEXT("got [%s], which has a gap between %d and %d. UK2Node_SwitchInteger stores no ")
                TEXT("case values — it renumbers its case pins to StartIndex, StartIndex+1, ... on ")
                TEXT("the next load — so a gapped set would silently change which arm each case runs. ")
                TEXT("Use a contiguous run, or dispatch the gapped values with branch / switch_string."),
                *Listed, OutSortedValues[Index - 1], OutSortedValues[Index]);
            return false;
        }
    }

    return true;
}

// Reports a compile error for a missing input exec pin on a target node.
// Used by both the ExecTargets loop and the ExecGoto wiring path; Context distinguishes
// the two in the diagnostic ("Target node" vs "ExecGoto target node").
// UK2Node_SwitchEnum does not expose a `Default` exec pin — the engine treats every
// unwired enum exec output as the implicit default — so when BPIR specifies a
// `default -> @label` arm we manually wire every enum entry that isn't already
// covered by an explicit arm to the default target's exec input.
static bool WireSwitchEnumDefaultArm(
    UK2Node_SwitchEnum* SwitchNode,
    UEnum* EnumType,
    const TArray<FBpirExecTarget>& AllExecTargets,
    UEdGraphPin* DefaultTargetExecIn,
    int32 SourceLine,
    TArray<FCompileError>& AccumulatedErrors)
{
    if (!SwitchNode || !EnumType || !DefaultTargetExecIn)
    {
        return false;
    }

    // Build a set of explicit-arm pin names (both raw and StripEnumScope'd forms).
    TSet<FString> ExplicitNames;
    for (const FBpirExecTarget& T : AllExecTargets)
    {
        const FString Trimmed = T.PinName.TrimStartAndEnd();
        if (Trimmed.Equals(TEXT("default"), ESearchCase::IgnoreCase))
        {
            continue;
        }
        ExplicitNames.Add(Trimmed);
        const FString Unscoped = BlueprintHandlerUtils::StripEnumScope(Trimmed);
        if (!Unscoped.Equals(Trimmed, ESearchCase::CaseSensitive))
        {
            ExplicitNames.Add(Unscoped);
        }
    }

    bool bAnyErrors = false;

    // Enumerate every non-_MAX, non-Hidden enum entry not already covered.
    const int32 NumEntries = EnumType->NumEnums();
    for (int32 i = 0; i < NumEntries; ++i)
    {
        const FString FullName = EnumType->GetNameStringByIndex(i);
        if (FullName.IsEmpty())
        {
            continue;
        }
        if (BlueprintEnumHelpers::IsEnumMaxEntryName(FullName))
        {
            continue;
        }
        if (EnumType->HasMetaData(TEXT("Hidden"), i))
        {
            continue;
        }

        const FString Unscoped = BlueprintHandlerUtils::StripEnumScope(FullName);

        // Skip entries that already have an explicit arm.
        const FString Scoped = FString::Printf(TEXT("%s::%s"), *EnumType->GetName(), *Unscoped);
        if (ExplicitNames.Contains(FullName)
            || ExplicitNames.Contains(Unscoped)
            || ExplicitNames.Contains(Scoped))
        {
            continue;
        }

        UEdGraphPin* ExecOut = FindSwitchEnumEntryPin(SwitchNode, FullName);
        if (!ExecOut)
        {
            // Defensive: every valid enum entry should have a pin on SwitchEnum, skip if not.
            continue;
        }

        const UEdGraphSchema* Schema = ExecOut->GetSchema();
        if (!Schema)
        {
            continue;
        }
        if (!Schema->TryCreateConnection(ExecOut, DefaultTargetExecIn))
        {
            AccumulatedErrors.Add(FCompileError(SourceLine,
                FString::Printf(TEXT("TryCreateConnection failed wiring switch_enum default arm '%s' -> exec input"),
                    *ExecOut->PinName.ToString())));
            bAnyErrors = true;
        }
    }

    return !bAnyErrors;
}

static void ReportMissingTargetInputPin(
    const FEmittedNodeInfo* TargetEmit,
    int32 SourceLine,
    const FString& RequestedPin,
    const TCHAR* Context,
    TArray<FCompileError>& OutErrors)
{
    FString AvailablePins;
    if (TargetEmit && TargetEmit->Node)
    {
        TArray<FString> Names;
        for (UEdGraphPin* Pin : TargetEmit->Node->Pins)
        {
            if (Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                Names.Add(Pin->PinName.ToString());
            }
        }
        AvailablePins = FString::Join(Names, TEXT(", "));
    }
    const FString TargetNodeName = (TargetEmit && TargetEmit->Node)
        ? TargetEmit->Node->GetNodeTitle(ENodeTitleType::ListView).ToString()
        : FString(TEXT("<unknown>"));
    UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: %s '%s' has no input exec pin named '%s' (available: %s)"),
        SourceLine, Context, *TargetNodeName, *RequestedPin, *AvailablePins);
    OutErrors.Add(FCompileError(SourceLine,
        FString::Printf(TEXT("%s '%s' has no input exec pin named '%s' (available: %s)"),
            Context, *TargetNodeName, *RequestedPin, *AvailablePins)));
}

// When CreateCallFunctionNode is called with a pure function, InOutExecPin becomes nullptr
// (no exec pins found). This helper detects that and restores the exec chain so downstream
// impure nodes still get wired. Also patches Inst.Opcode to Pure so WireExecPins skips it.
static void RestoreExecIfPure(
    UK2Node_CallFunction* CallNode,
    UEdGraphPin*& InOutExecPin,
    UEdGraphPin* SavedExecPin,
    FEmittedNodeInfo& Emit,
    FBpirInstruction& Inst)
{
    if (CallNode && !InOutExecPin && !NodeHasExecPins(CallNode))
    {
        InOutExecPin = SavedExecPin;
        Emit.ExecOutputPin = nullptr;
        Inst.Opcode = EBpirOpcode::Pure;
    }
}

static void CollectFunctionBodyNodesPreservingTerminators(
    UEdGraph* Graph,
    TSet<UEdGraphNode*>& OutNodes)
{
    if (!Graph)
    {
        return;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node
            && !Node->IsA<UK2Node_FunctionEntry>()
            && !Node->IsA<UK2Node_FunctionResult>())
        {
            OutNodes.Add(Node);
        }
    }
}

static bool HasReusedMacroGraphSnapshot(const TArray<FBpirReusedMacroGraphSnapshot>& Snapshots, UEdGraph* Graph)
{
    for (const FBpirReusedMacroGraphSnapshot& Snapshot : Snapshots)
    {
        if (Snapshot.Graph.Get() == Graph)
        {
            return true;
        }
    }
    return false;
}

static void CaptureReusedMacroGraphSnapshot(TArray<FBpirReusedMacroGraphSnapshot>& Snapshots, UEdGraph* Graph)
{
    if (!Graph || HasReusedMacroGraphSnapshot(Snapshots, Graph))
    {
        return;
    }

    TSet<UObject*> NodesToExport;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            NodesToExport.Add(Node);
        }
    }

    FBpirReusedMacroGraphSnapshot Snapshot;
    Snapshot.Graph = Graph;
    FEdGraphUtilities::ExportNodesToText(NodesToExport, Snapshot.ExportedNodesText);
    Snapshots.Add(MoveTemp(Snapshot));
}

static void RestoreReusedMacroGraphSnapshots(UBlueprint* Blueprint, const TArray<FBpirReusedMacroGraphSnapshot>& Snapshots)
{
    if (!Blueprint || Snapshots.Num() == 0)
    {
        return;
    }

    for (const FBpirReusedMacroGraphSnapshot& Snapshot : Snapshots)
    {
        UEdGraph* Graph = Snapshot.Graph.Get();
        if (!Graph)
        {
            continue;
        }

        Graph->Modify();
        TArray<UEdGraphNode*> CurrentNodes = Graph->Nodes;
        for (UEdGraphNode* Node : CurrentNodes)
        {
            if (Node)
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
            }
        }

        if (!Snapshot.ExportedNodesText.IsEmpty())
        {
            TSet<UEdGraphNode*> ImportedNodes;
            FEdGraphUtilities::ImportNodesFromText(Graph, Snapshot.ExportedNodesText, ImportedNodes);
        }
    }
}

static void ClearMacroTunnelPins(UK2Node_Tunnel* Tunnel)
{
    if (!Tunnel)
    {
        return;
    }

    Tunnel->Modify();
    for (int32 PinIndex = Tunnel->UserDefinedPins.Num() - 1; PinIndex >= 0; --PinIndex)
    {
        Tunnel->RemoveUserDefinedPin(Tunnel->UserDefinedPins[PinIndex]);
    }
}

static void ClearMacroBodyPreservingTunnels(UBlueprint* Blueprint, UEdGraph* MacroGraph, UK2Node_Tunnel* EntryTunnel, UK2Node_Tunnel* ExitTunnel)
{
    if (!Blueprint || !MacroGraph)
    {
        return;
    }

    MacroGraph->Modify();
    TArray<UEdGraphNode*> NodesToRemove;
    for (UEdGraphNode* Node : MacroGraph->Nodes)
    {
        if (Node && Node != EntryTunnel && Node != ExitTunnel)
        {
            NodesToRemove.Add(Node);
        }
    }

    for (UEdGraphNode* Node : NodesToRemove)
    {
        FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
    }
}

static bool OptionalStringMatches(const TOptional<FString>& A, const TOptional<FString>& B)
{
    if (A.IsSet() != B.IsSet())
    {
        return false;
    }
    return !A.IsSet() || A.GetValue() == B.GetValue();
}

static bool StringPointerValueMatches(const FString* A, const FString* B)
{
    if ((A == nullptr) != (B == nullptr))
    {
        return false;
    }
    return !A || *A == *B;
}

static bool MacroCallerTextDefaultMatches(const FText& A, const FText& B)
{
    if (A.ToString() != B.ToString())
    {
        return false;
    }

    FName TableA;
    FName TableB;
    FString TableKeyA;
    FString TableKeyB;
    const bool bHasTableA = FTextInspector::GetTableIdAndKey(A, TableA, TableKeyA);
    const bool bHasTableB = FTextInspector::GetTableIdAndKey(B, TableB, TableKeyB);
    if (bHasTableA != bHasTableB)
    {
        return false;
    }
    if (bHasTableA && (TableA != TableB || TableKeyA != TableKeyB))
    {
        return false;
    }

    if (!OptionalStringMatches(FTextInspector::GetNamespace(A), FTextInspector::GetNamespace(B)))
    {
        return false;
    }
    if (!OptionalStringMatches(FTextInspector::GetKey(A), FTextInspector::GetKey(B)))
    {
        return false;
    }

    return StringPointerValueMatches(FTextInspector::GetSourceString(A), FTextInspector::GetSourceString(B));
}

static FBpirMacroCallerSnapshot CaptureMacroCallerSnapshot(UK2Node_MacroInstance* MacroInstance)
{
    FBpirMacroCallerSnapshot Snapshot;
    Snapshot.MacroInstance = MacroInstance;
    if (MacroInstance && MacroInstance->GetMacroGraph())
    {
        Snapshot.MacroName = MacroInstance->GetMacroGraph()->GetName();
    }

    if (!MacroInstance)
    {
        return Snapshot;
    }

    for (UEdGraphPin* Pin : MacroInstance->Pins)
    {
        if (!Pin)
        {
            continue;
        }

        FBpirMacroCallerPinSnapshot PinSnapshot;
        PinSnapshot.PinName = Pin->PinName;
        PinSnapshot.Direction = Pin->Direction;
        PinSnapshot.PinType = Pin->PinType;
        PinSnapshot.DefaultValue = Pin->DefaultValue;
        PinSnapshot.DefaultTextValue = Pin->DefaultTextValue;
        PinSnapshot.DefaultObject = Pin->DefaultObject;

        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (!LinkedPin || !LinkedPin->GetOwningNode())
            {
                continue;
            }

            FBpirMacroCallerLinkSnapshot LinkSnapshot;
            LinkSnapshot.LinkedNode = LinkedPin->GetOwningNode();
            LinkSnapshot.LinkedPinName = LinkedPin->PinName;
            LinkSnapshot.LinkedDirection = LinkedPin->Direction;
            PinSnapshot.Links.Add(MoveTemp(LinkSnapshot));
        }

        Snapshot.Pins.Add(MoveTemp(PinSnapshot));
    }

    return Snapshot;
}

static bool MacroInterfaceContainsCallerPin(UEdGraph* MacroGraph, const FBpirMacroCallerPinSnapshot& PinSnapshot)
{
    UK2Node_Tunnel* EntryTunnel = nullptr;
    UK2Node_Tunnel* ExitTunnel = nullptr;
    if (!BlueprintHandlerUtils::FindMacroTunnelPair(MacroGraph, EntryTunnel, ExitTunnel))
    {
        return false;
    }

    if (PinSnapshot.Direction == EGPD_Input)
    {
        return EntryTunnel && EntryTunnel->FindPin(PinSnapshot.PinName, EGPD_Output) != nullptr;
    }
    if (PinSnapshot.Direction == EGPD_Output)
    {
        return ExitTunnel && ExitTunnel->FindPin(PinSnapshot.PinName, EGPD_Input) != nullptr;
    }
    return false;
}

static bool PinHasSnapshotLink(UEdGraphPin* Pin, const FBpirMacroCallerLinkSnapshot& LinkSnapshot)
{
    if (!Pin)
    {
        return false;
    }

    UEdGraphNode* LinkedNode = LinkSnapshot.LinkedNode.Get();
    if (!LinkedNode)
    {
        return false;
    }

    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        if (LinkedPin
            && LinkedPin->GetOwningNode() == LinkedNode
            && LinkedPin->PinName == LinkSnapshot.LinkedPinName
            && LinkedPin->Direction == LinkSnapshot.LinkedDirection)
        {
            return true;
        }
    }
    return false;
}

static bool ValidateMacroCallerSnapshotPreserved(
    const FBpirMacroCallerSnapshot& Snapshot,
    FString& OutError)
{
    UK2Node_MacroInstance* MacroInstance = Snapshot.MacroInstance.Get();
    if (!MacroInstance)
    {
        return true;
    }

    for (const FBpirMacroCallerPinSnapshot& PinSnapshot : Snapshot.Pins)
    {
        UEdGraphPin* Pin = MacroInstance->FindPin(PinSnapshot.PinName, PinSnapshot.Direction);
        if (!Pin)
        {
            OutError = FString::Printf(TEXT("Recompiling macro '%s' removed existing caller pin '%s'."),
                *Snapshot.MacroName,
                *PinSnapshot.PinName.ToString());
            return false;
        }

        if (Pin->PinType != PinSnapshot.PinType)
        {
            OutError = FString::Printf(TEXT("Recompiling macro '%s' changed type on existing caller pin '%s'."),
                *Snapshot.MacroName,
                *PinSnapshot.PinName.ToString());
            return false;
        }

        const bool bDefaultChanged =
            Pin->DefaultValue != PinSnapshot.DefaultValue
            || !MacroCallerTextDefaultMatches(Pin->DefaultTextValue, PinSnapshot.DefaultTextValue)
            || Pin->DefaultObject != PinSnapshot.DefaultObject.Get();
        if (bDefaultChanged)
        {
            OutError = FString::Printf(TEXT("Recompiling macro '%s' changed default value on existing caller pin '%s'."),
                *Snapshot.MacroName,
                *PinSnapshot.PinName.ToString());
            return false;
        }

        if (Pin->LinkedTo.Num() != PinSnapshot.Links.Num())
        {
            OutError = FString::Printf(TEXT("Recompiling macro '%s' changed link count on existing caller pin '%s'."),
                *Snapshot.MacroName,
                *PinSnapshot.PinName.ToString());
            return false;
        }

        for (const FBpirMacroCallerLinkSnapshot& LinkSnapshot : PinSnapshot.Links)
        {
            if (!PinHasSnapshotLink(Pin, LinkSnapshot))
            {
                OutError = FString::Printf(TEXT("Recompiling macro '%s' broke an existing caller link on pin '%s'."),
                    *Snapshot.MacroName,
                    *PinSnapshot.PinName.ToString());
                return false;
            }
        }
    }

    return true;
}

static void RestoreMacroCallerSnapshots(const TArray<FBpirMacroCallerSnapshot>& Snapshots)
{
    for (const FBpirMacroCallerSnapshot& Snapshot : Snapshots)
    {
        UK2Node_MacroInstance* MacroInstance = Snapshot.MacroInstance.Get();
        if (!MacroInstance)
        {
            continue;
        }

        MacroInstance->Modify();
        for (const FBpirMacroCallerPinSnapshot& PinSnapshot : Snapshot.Pins)
        {
            UEdGraphPin* Pin = MacroInstance->FindPin(PinSnapshot.PinName, PinSnapshot.Direction);
            if (!Pin)
            {
                continue;
            }

            Pin->Modify();
            Pin->BreakAllPinLinks();
            Pin->DefaultValue = PinSnapshot.DefaultValue;
            Pin->DefaultTextValue = PinSnapshot.DefaultTextValue;
            Pin->DefaultObject = PinSnapshot.DefaultObject.Get();

            const UEdGraphSchema* Schema = Pin->GetSchema();
            for (const FBpirMacroCallerLinkSnapshot& LinkSnapshot : PinSnapshot.Links)
            {
                UEdGraphNode* LinkedNode = LinkSnapshot.LinkedNode.Get();
                UEdGraphPin* LinkedPin = LinkedNode
                    ? LinkedNode->FindPin(LinkSnapshot.LinkedPinName, LinkSnapshot.LinkedDirection)
                    : nullptr;
                if (Schema && LinkedPin)
                {
                    Schema->TryCreateConnection(Pin, LinkedPin);
                }
            }
        }
    }
}

static void ReconstructMacroCallersFromSnapshots(const TArray<FBpirMacroCallerSnapshot>& Snapshots)
{
    for (const FBpirMacroCallerSnapshot& Snapshot : Snapshots)
    {
        if (UK2Node_MacroInstance* MacroInstance = Snapshot.MacroInstance.Get())
        {
            MacroInstance->Modify();
            MacroInstance->ReconstructNode();
        }
    }
}

static bool ReconstructSameBlueprintMacroCallers(
    UBlueprint* Blueprint,
    const TSet<UEdGraph*>& MacroGraphs,
    const TSet<FGuid>& CreatedNodeGuids,
    TArray<FCompileError>& OutErrors,
    TArray<FBpirMacroCallerSnapshot>& OutCallerSnapshots)
{
    if (!Blueprint || MacroGraphs.Num() == 0)
    {
        return true;
    }

    const TArray<UEdGraph*> Graphs = BlueprintHandlerUtils::CollectAllBlueprintGraphsRecursive(Blueprint);
    OutCallerSnapshots.Reset();

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_MacroInstance* MacroInstance = Cast<UK2Node_MacroInstance>(Node);
            if (!MacroInstance
                || !MacroGraphs.Contains(MacroInstance->GetMacroGraph())
                || CreatedNodeGuids.Contains(MacroInstance->NodeGuid))
            {
                continue;
            }

            FBpirMacroCallerSnapshot Snapshot = CaptureMacroCallerSnapshot(MacroInstance);
            for (const FBpirMacroCallerPinSnapshot& PinSnapshot : Snapshot.Pins)
            {
                if (!MacroInterfaceContainsCallerPin(MacroInstance->GetMacroGraph(), PinSnapshot))
                {
                    OutErrors.Add(FCompileError(-1, FString::Printf(
                        TEXT("Recompiling macro '%s' would remove existing caller pin '%s'."),
                        *Snapshot.MacroName,
                        *PinSnapshot.PinName.ToString())));
                    return false;
                }
            }
            OutCallerSnapshots.Add(MoveTemp(Snapshot));
        }
    }

    for (const FBpirMacroCallerSnapshot& Snapshot : OutCallerSnapshots)
    {
        UK2Node_MacroInstance* MacroInstance = Snapshot.MacroInstance.Get();
        if (!MacroInstance)
        {
            continue;
        }

        MacroInstance->Modify();
        MacroInstance->ReconstructNode();

        FString Error;
        if (!ValidateMacroCallerSnapshotPreserved(Snapshot, Error))
        {
            OutErrors.Add(FCompileError(-1, Error));
            return false;
        }
    }

    return true;
}

using BpirAnimGraphFamily::IsAnimGraphFamily;

static UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
        {
            return EntryNode;
        }
    }
    return nullptr;
}

static bool HasExplicitOverrideSignature(const FBpirEntryBlock& Block)
{
    return Block.Params.Num() > 0 || !Block.ReturnType.IsEmpty() || Block.OutputParams.Num() > 0;
}

static bool ValidateNoDuplicatePlainEntryEvents(
    const TArray<FBpirEntryBlock>& Blocks,
    TArray<FCompileError>& OutErrors)
{
    TMap<FString, FString> FirstEventNameByKey;
    bool bValid = true;

    for (const FBpirEntryBlock& Block : Blocks)
    {
        if (Block.Kind != EBpirEntryKind::Event)
        {
            continue;
        }

        const FString Key = Block.Name.ToLower();
        if (const FString* FirstName = FirstEventNameByKey.Find(Key))
        {
            OutErrors.Add(FCompileError(
                -1,
                FString::Printf(
                    TEXT("Duplicate plain entry events named '%s'. Plain entry event blocks cannot represent multiple bound delegates with the same delegate signature; use entry widget_event Component.Delegate(...) or entry component_event Component.Delegate(...) for bound delegates."),
                    **FirstName)));
            bValid = false;
            continue;
        }

        FirstEventNameByKey.Add(Key, Block.Name);
    }

    return bValid;
}

// Build a "Could not find target pin 'X' on node 'Y'" message augmented with
// the node's real input pin list and a best-match "Did you mean 'Z'?" hint.
// Authors hit pin-name errors when they guess (e.g. "WidgetType" vs "Class" on
// K2Node_CreateWidget); exposing the real pins on-error saves a decompile
// round-trip to discover them.
static FString BuildMissingPinHint(UEdGraphNode* Node, const FString& RequestedPinName)
{
    const FString NodeTitle = Node
        ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString()
        : FString(TEXT("null"));

    TArray<FString> AvailablePins;
    FString BestGuess;
    if (Node)
    {
        const FString NormalizedArg = RequestedPinName.Replace(TEXT(" "), TEXT("")).ToLower();
        int32 BestScore = INT32_MAX;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
            const FString PinStr = Pin->PinName.ToString();
            AvailablePins.Add(PinStr);

            // Substring/containment match with length-delta tiebreak — good
            // enough to surface near-misses without a full edit-distance pass.
            const FString NormalizedPin = PinStr.Replace(TEXT(" "), TEXT("")).ToLower();
            if (!NormalizedArg.IsEmpty()
                && (NormalizedPin.Contains(NormalizedArg) || NormalizedArg.Contains(NormalizedPin)))
            {
                const int32 Score = FMath::Abs(NormalizedPin.Len() - NormalizedArg.Len());
                if (Score < BestScore)
                {
                    BestScore = Score;
                    BestGuess = PinStr;
                }
            }
        }
    }

    FString Out = FString::Printf(
        TEXT("Could not find target pin '%s' on node '%s'"),
        *RequestedPinName, *NodeTitle);
    if (!BestGuess.IsEmpty())
    {
        Out += FString::Printf(TEXT(". Did you mean '%s'?"), *BestGuess);
    }
    if (AvailablePins.Num() > 0)
    {
        Out += FString::Printf(TEXT(" Available pins: %s"),
            *FString::Join(AvailablePins, TEXT(", ")));
    }
    return Out;
}

static bool BuildExpectedOverrideSignature(
    const FBpirEntryBlock& Block,
    TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor>& OutInputs,
    TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor>& OutOutputs,
    FString& OutErrorMessage)
{
    OutInputs.Reset();
    OutOutputs.Reset();
    OutErrorMessage.Reset();

    for (const FBpirEntryBlock::FParam& Param : Block.Params)
    {
        BlueprintHandlerUtils::FNamedPinTypeDescriptor Descriptor;
        if (!BlueprintHandlerUtils::BuildNamedPinDescriptor(Param.Name, Param.Type, Descriptor))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Invalid override input parameter '%s %s'"),
                *BpirTypeSpecParser::TypeSpecToBpirText(Param.Type),
                *Param.Name);
            return false;
        }
        OutInputs.Add(Descriptor);
    }

    if (!Block.ReturnType.IsEmpty() && !Block.ReturnType.IsVoid())
    {
        BlueprintHandlerUtils::FNamedPinTypeDescriptor ReturnDescriptor;
        if (!BlueprintHandlerUtils::BuildNamedPinDescriptor(TEXT("ReturnValue"), Block.ReturnType, ReturnDescriptor))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Invalid override return type '%s'"),
                *BpirTypeSpecParser::TypeSpecToBpirText(Block.ReturnType));
            return false;
        }
        OutOutputs.Add(ReturnDescriptor);
    }

    for (const FBpirEntryBlock::FParam& Param : Block.OutputParams)
    {
        BlueprintHandlerUtils::FNamedPinTypeDescriptor Descriptor;
        if (!BlueprintHandlerUtils::BuildNamedPinDescriptor(Param.Name, Param.Type, Descriptor))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Invalid override output parameter '%s %s'"),
                *BpirTypeSpecParser::TypeSpecToBpirText(Param.Type),
                *Param.Name);
            return false;
        }
        OutOutputs.Add(Descriptor);
    }

    return true;
}

static void ParseFloatCurveKeyframes(const FString& Value, UCurveFloat* CurveFloat)
{
    if (!CurveFloat) return;

    // Parse float_curve((t1,v1),(t2,v2),...) format
    FString Inner = Value;
    int32 ParenStart = Inner.Find(TEXT("("), ESearchCase::IgnoreCase, ESearchDir::FromStart, Inner.Find(TEXT("float_curve")));
    if (ParenStart != INDEX_NONE)
    {
        Inner = Inner.Mid(ParenStart + 1);
        // Remove trailing )
        if (Inner.EndsWith(TEXT(")")))
        {
            Inner.LeftChopInline(1);
        }
    }

    // Parse each (time,value) pair
    FString Remaining = Inner;
    while (!Remaining.IsEmpty())
    {
        int32 OpenParen = Remaining.Find(TEXT("("));
        if (OpenParen == INDEX_NONE) break;
        int32 CloseParen = Remaining.Find(TEXT(")"), ESearchCase::IgnoreCase, ESearchDir::FromStart, OpenParen);
        if (CloseParen == INDEX_NONE) break;

        FString Pair = Remaining.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
        Remaining = Remaining.Mid(CloseParen + 1);

        FString TimeStr, ValueStr;
        if (Pair.Split(TEXT(","), &TimeStr, &ValueStr))
        {
            float Time = FCString::Atof(*TimeStr.TrimStartAndEnd());
            float Val = FCString::Atof(*ValueStr.TrimStartAndEnd());
            CurveFloat->FloatCurve.AddKey(Time, Val);
        }
    }
}

// ----------------------------------------------------------------------------
// FBpirEntryMetadata application helpers
// ----------------------------------------------------------------------------

// Map FBpirEntryMetadata::EAccess to the matching FUNC_* access bit.
static int32 BpirAccessToFunctionFlag(FBpirEntryMetadata::EAccess Access)
{
    switch (Access)
    {
    case FBpirEntryMetadata::EAccess::Public:    return FUNC_Public;
    case FBpirEntryMetadata::EAccess::Protected: return FUNC_Protected;
    case FBpirEntryMetadata::EAccess::Private:   return FUNC_Private;
    case FBpirEntryMetadata::EAccess::Default:
    default:                                     return 0;
    }
}

// Compute a new flag word from an existing one by applying the @flags(...) decorator.
// Always clears FUNC_AccessSpecifiers / FUNC_BlueprintPure / FUNC_Const / FUNC_Exec
// before re-setting them so a recompile drops bits removed from the decorator.
// Also masks FUNC_Native unconditionally (UK2Node_FunctionEntry::SetExtraFlags does
// the same on the entry-flag setter side; matching here keeps custom-event behavior
// consistent).
static int32 ApplyFlagsDecoratorToFunctionFlags(int32 ExistingFlags, const FBpirEntryMetadata& Metadata)
{
    int32 NewFlags = ExistingFlags
        & ~(FUNC_AccessSpecifiers | FUNC_BlueprintPure | FUNC_Const | FUNC_Exec | FUNC_Native);

    NewFlags |= BpirAccessToFunctionFlag(Metadata.Access);
    if (Metadata.bPure) { NewFlags |= FUNC_BlueprintPure; }
    if (Metadata.bConst) { NewFlags |= FUNC_Const; }
    if (Metadata.bExec) { NewFlags |= FUNC_Exec; }

    return NewFlags;
}

// Apply the five FText/FString @meta(...) fields onto a FKismetUserDeclaredFunctionMetadata.
// Category is intentionally NOT written here — the caller routes through
// FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory so the Blueprint-level
// FunctionCategories list stays in sync.
static void ApplyMetaDecoratorToKismetMetadata(FKismetUserDeclaredFunctionMetadata& OutMeta, const FBpirEntryMetadata& Metadata)
{
    OutMeta.ToolTip = Metadata.Tooltip;
    OutMeta.Keywords = Metadata.Keywords;
    OutMeta.CompactNodeTitle = Metadata.CompactNodeTitle;
    OutMeta.DeprecationMessage = Metadata.DeprecationMessage;
}

// Apply the bool @flags(...) members onto a FKismetUserDeclaredFunctionMetadata.
// Assigned unconditionally (not OR'd) so a recompile drops cleared bits.
static void ApplyFlagsDecoratorToKismetMetadata(FKismetUserDeclaredFunctionMetadata& OutMeta, const FBpirEntryMetadata& Metadata)
{
    OutMeta.bCallInEditor = Metadata.bCallInEditor;
    OutMeta.bThreadSafe = Metadata.bThreadSafe;
    OutMeta.bIsUnsafeDuringActorConstruction = Metadata.bUnsafeDuringActorConstruction;
    OutMeta.bIsDeprecated = Metadata.bDeprecated;
}

// Apply both decorators to a UK2Node_FunctionEntry plus the owning function
// graph (so Category routes through the Blueprint-level FunctionCategories list).
// Always writes the typed fields and flag bits from Metadata so a default-constructed
// FBpirEntryMetadata (no-decorator case) reliably resets stale state from a prior compile.
static void ApplyEntryMetadataToFunctionEntry(
    UK2Node_FunctionEntry* EntryNode,
    UEdGraph* FunctionGraph,
    const FBpirEntryMetadata& Metadata)
{
    if (!EntryNode) { return; }

    ApplyMetaDecoratorToKismetMetadata(EntryNode->MetaData, Metadata);

    // Category routes through the Blueprint-level helper so FunctionCategories stays
    // in sync; the helper writes MetaData.Category internally. The helper rebuilds the
    // Blueprint-level FunctionCategories list and marks structural modification, so
    // only invoke when the value actually changes — otherwise a 369-function recompile
    // pays for 369 list rebuilds with no behavior change.
    if (FunctionGraph)
    {
        if (!EntryNode->MetaData.Category.EqualTo(Metadata.Category))
        {
            FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(
                FunctionGraph, Metadata.Category, /*bDontRecompile=*/true);
        }
    }
    else
    {
        EntryNode->MetaData.Category = Metadata.Category;
    }

    // SetExtraFlags masks FUNC_Native on the way in; ApplyFlagsDecoratorToFunctionFlags
    // pre-masks it as well so the recompile path doesn't leak the bit.
    const int32 NewFlags = ApplyFlagsDecoratorToFunctionFlags(EntryNode->GetExtraFlags(), Metadata);
    EntryNode->SetExtraFlags(NewFlags);

    ApplyFlagsDecoratorToKismetMetadata(EntryNode->MetaData, Metadata);
}

// Apply both decorators to a UK2Node_CustomEvent. Same shape as
// ApplyEntryMetadataToFunctionEntry but writes FunctionFlags (the uint32 EFunctionFlags
// bag on UK2Node_Event) instead of ExtraFlags. Always writes the typed fields and flag
// bits so a default-constructed FBpirEntryMetadata reliably resets stale state from a
// prior compile.
static void ApplyEntryMetadataToCustomEvent(
    UK2Node_CustomEvent* EventNode,
    const FBpirEntryMetadata& Metadata)
{
    if (!EventNode) { return; }

    ApplyMetaDecoratorToKismetMetadata(EventNode->GetUserDefinedMetaData(), Metadata);
    EventNode->GetUserDefinedMetaData().Category = Metadata.Category;

    EventNode->FunctionFlags = static_cast<uint32>(
        ApplyFlagsDecoratorToFunctionFlags(static_cast<int32>(EventNode->FunctionFlags), Metadata));
    ApplyFlagsDecoratorToKismetMetadata(EventNode->GetUserDefinedMetaData(), Metadata);

    // UK2Node_CustomEvent shadows three of the metadata fields with public UPROPERTYs
    // (DeprecationMessage / bIsDeprecated / bCallInEditor) that the editor UI reads
    // and writes directly — they are the editor-set source of truth for these three
    // values (and the decompiler reads from them for the same reason). Mirror the
    // metadata writes onto those node-level copies so the round-trip is symmetric
    // and the editor UI reflects the compiled state.
    EventNode->DeprecationMessage = Metadata.DeprecationMessage;
    EventNode->bIsDeprecated = Metadata.bDeprecated;
    EventNode->bCallInEditor = Metadata.bCallInEditor;
}

// Apply @meta(...) to a macro entry tunnel. @flags is rejected by the parser for
// macros, so this never writes flag bits; checkSlow guards the invariant defensively.
// Always writes the typed fields so a default-constructed FBpirEntryMetadata reliably
// resets stale state from a prior compile.
static void ApplyEntryMetadataToMacroTunnel(
    UK2Node_Tunnel* EntryTunnel,
    const FBpirEntryMetadata& Metadata)
{
    if (!EntryTunnel) { return; }
    checkSlow(!Metadata.bFlagsPresent);

    ApplyMetaDecoratorToKismetMetadata(EntryTunnel->MetaData, Metadata);
    EntryTunnel->MetaData.Category = Metadata.Category;
}

// ----------------------------------------------------------------------------
// Layout pass helper
// ----------------------------------------------------------------------------

// Run the layout pass over nodes created during compilation. Handles multi-graph
// compilations (main event graph + function/macro graphs).
static void RunLayoutPass(
    UBlueprint* Blueprint,
    const TArray<FGuid>& CreatedGUIDs,
    UEdGraph* MainGraph,
    const TArray<UEdGraph*>& ExtraGraphs,
    UEdGraphNode* AnchorHint = nullptr)
{
    if (!Blueprint || CreatedGUIDs.Num() == 0)
    {
        return;
    }

    // Build a set of all GUIDs for quick lookup.
    TSet<FGuid> GUIDSet;
    GUIDSet.Reserve(CreatedGUIDs.Num());
    for (const FGuid& G : CreatedGUIDs)
    {
        GUIDSet.Add(G);
    }

    // Gather all unique graphs to process.
    TArray<UEdGraph*> Graphs;
    if (MainGraph)
    {
        Graphs.AddUnique(MainGraph);
    }
    for (UEdGraph* G : ExtraGraphs)
    {
        if (G)
        {
            Graphs.AddUnique(G);
        }
    }

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }

        TArray<UEdGraphNode*> Pool;
        TArray<UEdGraphNode*> Obstacles;
        UEdGraphNode* Anchor = nullptr;

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (GUIDSet.Contains(Node->NodeGuid))
            {
                Pool.Add(Node);
                // Use the first event/entry/tunnel node in the pool as anchor.
                if (!Anchor)
                {
                    if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>()
                        || Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Tunnel>())
                    {
                        Anchor = Node;
                    }
                }
            }
            else
            {
                Obstacles.Add(Node);
            }
        }

        // Prefer the provided anchor hint when it's in this graph.
        if (AnchorHint && Graph->Nodes.Contains(AnchorHint))
        {
            Anchor = AnchorHint;
        }

        if (Pool.Num() == 0)
        {
            continue;
        }

        // For pool nodes with no entry-type anchor, walk upstream along input exec
        // pins to find the true exec root — even if that root is an existing obstacle.
        if (!Anchor)
        {
            Anchor = Pool[0];
            TSet<UEdGraphNode*> WalkVisited;
            UEdGraphNode* Cursor = Anchor;
            while (Cursor && !WalkVisited.Contains(Cursor))
            {
                WalkVisited.Add(Cursor);
                UEdGraphNode* Upstream = nullptr;
                for (UEdGraphPin* Pin : Cursor->Pins)
                {
                    if (!Pin || Pin->bHidden) continue;
                    if (Pin->Direction != EGPD_Input) continue;
                    if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        if (Linked && Linked->GetOwningNodeUnchecked())
                        {
                            Upstream = Linked->GetOwningNodeUnchecked();
                            break;
                        }
                    }
                    if (Upstream) break;
                }
                if (!Upstream) break;
                Cursor = Upstream;
            }
            if (Cursor && Cursor != Anchor)
            {
                Anchor = Cursor;
                Pool.AddUnique(Cursor);
                Obstacles.RemoveAll([Cursor](UEdGraphNode* N) { return N == Cursor; });
            }
        }

        BpirLayout::FNodeLayoutEngine LayoutEngine(Graph, Pool, Anchor, Obstacles);
        LayoutEngine.Format();
    }
}

// ----------------------------------------------------------------------------
// Constructor / Destructor
// ----------------------------------------------------------------------------

FBpirCompiler::FBpirCompiler(UBlueprint* InTargetBlueprint)
    : TargetBlueprint(InTargetBlueprint)
    , CurrentGraph(nullptr)
{
    check(InTargetBlueprint);

    // Use the blueprint's event graph as the default working graph
    if (InTargetBlueprint->UbergraphPages.Num() > 0)
    {
        CurrentGraph = InTargetBlueprint->UbergraphPages[0];
    }

    NodeEmitter      = MakeUnique<FCodeNodeEmitter>(InTargetBlueprint, CurrentGraph);
    PinResolver      = MakeUnique<FCodePinResolver>();
    FunctionResolver = MakeUnique<FCodeFunctionResolver>();
    ValueResolver    = MakeUnique<FBpirValueResolver>(*NodeEmitter, *PinResolver, InTargetBlueprint, CurrentGraph);

    // Map user-facing BPIR event names to the UE function names they bind to
    EventNameMap.Add(TEXT("BeginPlay"),          FName(TEXT("ReceiveBeginPlay")));
    EventNameMap.Add(TEXT("Tick"),               FName(TEXT("ReceiveTick")));
    EventNameMap.Add(TEXT("ActorBeginOverlap"),  FName(TEXT("ReceiveActorBeginOverlap")));
    EventNameMap.Add(TEXT("ActorEndOverlap"),    FName(TEXT("ReceiveActorEndOverlap")));
    EventNameMap.Add(TEXT("Destroyed"),          FName(TEXT("ReceiveDestroyed")));
    EventNameMap.Add(TEXT("AnyDamage"),          FName(TEXT("ReceiveAnyDamage")));
    EventNameMap.Add(TEXT("Hit"),                FName(TEXT("ReceiveHit")));
    EventNameMap.Add(TEXT("EndPlay"),            FName(TEXT("ReceiveEndPlay")));
    EventNameMap.Add(TEXT("PointDamage"),        FName(TEXT("ReceivePointDamage")));
    EventNameMap.Add(TEXT("RadialDamage"),       FName(TEXT("ReceiveRadialDamage")));
}

FBpirCompiler::~FBpirCompiler()
{
    // TUniquePtr members clean themselves up
}

FName FBpirCompiler::ResolveOverrideEventName(const FString& BaseName, UClass* ParentClass) const
{
    const FName LiteralName(*BaseName);
    if (ParentClass && ParentClass->FindFunctionByName(LiteralName) != nullptr)
    {
        return LiteralName;
    }
    if (ParentClass && ParentClass->IsChildOf(AActor::StaticClass()))
    {
        if (const FName* Mapped = EventNameMap.Find(BaseName))
        {
            return *Mapped;
        }
    }
    return LiteralName;
}

// ----------------------------------------------------------------------------
// Test Hook
// ----------------------------------------------------------------------------

void FBpirCompiler::RunLayoutPassForTest(
    UBlueprint* Blueprint,
    const TArray<FGuid>& CreatedGUIDs,
    UEdGraph* MainGraph,
    const TArray<UEdGraph*>& ExtraGraphs,
    UEdGraphNode* AnchorHint)
{
    RunLayoutPass(Blueprint, CreatedGUIDs, MainGraph, ExtraGraphs, AnchorHint);
}

struct FAuthoredPlacementMode
{
    bool bEnabled = false;
    TSet<FGuid> PrimaryNodeGuids;
};

static FString GetBpirEntryBodyName(const FBpirEntryBlock& Block)
{
    if (!Block.ComponentName.IsEmpty())
    {
        return FString::Printf(TEXT("%s.%s"), *Block.ComponentName, *Block.Name);
    }
    if (!Block.Name.IsEmpty())
    {
        return Block.Name;
    }
    return TEXT("<body>");
}

static FString GetBpirInstructionLabel(const FBpirInstruction& Inst)
{
    if (!Inst.ResultName.IsEmpty())
    {
        return FString::Printf(TEXT("%%%s"), *Inst.ResultName);
    }
    if (!Inst.FunctionName.IsEmpty())
    {
        return Inst.FunctionName;
    }
    if (!Inst.TypeArg.IsEmpty())
    {
        return Inst.TypeArg;
    }
    return FString::Printf(TEXT("opcode %d"), static_cast<int32>(Inst.Opcode));
}

static FString GetBpirManualPlacementRemediation()
{
    return TEXT("Fix: add @(x, y) to every visible node-backed instruction in the body, remove all positions from the body to use auto-layout, or introduce an explicit positioned BPIR instruction for the helper.");
}

static void AddBpirManualPlacementError(
    TArray<FCompileError>& Errors,
    const FBpirEntryBlock& Block,
    const FBpirInstruction& Inst,
    const FString& Reason)
{
    Errors.Add(FCompileError(
        Inst.SourceLine,
        FString::Printf(
            TEXT("Manual BPIR placement error in entry body '%s' at source line %d (%s): %s %s"),
            *GetBpirEntryBodyName(Block),
            Inst.SourceLine,
            *GetBpirInstructionLabel(Inst),
            *Reason,
            *GetBpirManualPlacementRemediation())));
}

static FAuthoredPlacementMode ApplyAuthoredPositionsForBlock(
    const FBpirEntryBlock& Block,
    const TMap<int32, FEmittedNodeInfo>& EmitMap,
    TArray<FCompileError>& Errors)
{
    FAuthoredPlacementMode Result;
    int32 PrimaryInstructionCount = 0;
    int32 PositionedPrimaryInstructionCount = 0;

    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        const FBpirInstruction& Inst = Block.Instructions[i];
        const FEmittedNodeInfo* Info = EmitMap.Find(i);
        const bool bHasPrimaryNode = Info && Info->Node;

        if (bHasPrimaryNode)
        {
            ++PrimaryInstructionCount;
            if (Inst.bHasAuthoredPosition)
            {
                ++PositionedPrimaryInstructionCount;
            }
        }
        else if (Inst.bHasAuthoredPosition)
        {
            AddBpirManualPlacementError(
                Errors,
                Block,
                Inst,
                TEXT("@(x, y) only applies to visible node-backed BPIR instructions; this instruction emitted no primary graph node."));
        }
    }

    if (PrimaryInstructionCount == 0 || PositionedPrimaryInstructionCount == 0)
    {
        return Result;
    }

    if (PositionedPrimaryInstructionCount != PrimaryInstructionCount)
    {
        for (int32 i = 0; i < Block.Instructions.Num(); ++i)
        {
            const FBpirInstruction& Inst = Block.Instructions[i];
            const FEmittedNodeInfo* Info = EmitMap.Find(i);
            if (Info && Info->Node && !Inst.bHasAuthoredPosition)
            {
                AddBpirManualPlacementError(
                    Errors,
                    Block,
                    Inst,
                    TEXT("mixed authored-position mode is forbidden because every visible node-backed instruction in one entry body must either all have @(x, y) or none do."));
            }
        }
        return Result;
    }

    Result.bEnabled = true;
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        const FBpirInstruction& Inst = Block.Instructions[i];
        const FEmittedNodeInfo* Info = EmitMap.Find(i);
        if (!Info || !Info->Node)
        {
            continue;
        }

        Info->Node->NodePosX = FMath::RoundToInt(Inst.AuthoredPosition.X);
        Info->Node->NodePosY = FMath::RoundToInt(Inst.AuthoredPosition.Y);
        Result.PrimaryNodeGuids.Add(Info->Node->NodeGuid);
    }

    return Result;
}

static void ApplyAuthoredEntryPositionForBlock(const FBpirEntryBlock& Block, UEdGraphNode* EntryNode)
{
    if (!Block.bHasAuthoredEntryPosition || !EntryNode)
    {
        return;
    }

    EntryNode->NodePosX = FMath::RoundToInt(Block.AuthoredEntryPosition.X);
    EntryNode->NodePosY = FMath::RoundToInt(Block.AuthoredEntryPosition.Y);
}

static void ApplyBpirEnabledState(
    EBpirNodeEnabledState State,
    bool bHasState,
    UEdGraphNode* Node)
{
    if (!bHasState || !Node)
    {
        return;
    }

    switch (State)
    {
    case EBpirNodeEnabledState::Disabled:
        Node->SetEnabledState(ENodeEnabledState::Disabled, /*bUserAction=*/true);
        break;
    case EBpirNodeEnabledState::DevelopmentOnly:
        Node->SetEnabledState(ENodeEnabledState::DevelopmentOnly, /*bUserAction=*/true);
        break;
    case EBpirNodeEnabledState::Enabled:
    default:
        Node->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction=*/true);
        break;
    }
}

static bool IsEntryNodeForBlock(const FBpirEntryBlock& Block, UEdGraphNode* Node)
{
    if (!Node)
    {
        return false;
    }

    switch (Block.Kind)
    {
    case EBpirEntryKind::CustomEvent:
        if (UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(Node))
        {
            return EventNode->CustomFunctionName.ToString().Equals(Block.Name, ESearchCase::IgnoreCase);
        }
        return false;

    case EBpirEntryKind::ComponentEvent:
    case EBpirEntryKind::WidgetEvent:
        if (UK2Node_ComponentBoundEvent* EventNode = Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            return EventNode->ComponentPropertyName.ToString().Equals(Block.ComponentName, ESearchCase::IgnoreCase)
                && EventNode->DelegatePropertyName.ToString().Equals(Block.Name, ESearchCase::IgnoreCase);
        }
        return false;

    case EBpirEntryKind::Function:
    case EBpirEntryKind::Override:
        if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
        {
            return EntryNode->GetGraph()
                && EntryNode->GetGraph()->GetName().Equals(Block.Name, ESearchCase::IgnoreCase);
        }
        if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
        {
            return EventNode->EventReference.GetMemberName().ToString().Equals(Block.Name, ESearchCase::IgnoreCase);
        }
        return false;

    case EBpirEntryKind::Construction:
        if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
        {
            return EntryNode->GetGraph()
                && EntryNode->GetGraph()->GetName() == TEXT("UserConstructionScript");
        }
        return false;

    case EBpirEntryKind::KeyPressed:
    case EBpirEntryKind::KeyReleased:
        return FBpirInputKeyHelpers::IsInputKeyNodeForBpirEntry(
            Cast<UK2Node_InputKey>(Node),
            Block.Name,
            Block.Kind == EBpirEntryKind::KeyReleased);

    case EBpirEntryKind::InputAction:
        if (const UInputAction* InputAction = FCodeNodeEmitter::GetEnhancedInputAction(Node))
        {
            return InputAction->GetPathName().Equals(Block.Name, ESearchCase::CaseSensitive);
        }
        return false;

    case EBpirEntryKind::Macro:
        if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
        {
            return Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs;
        }
        return false;

    case EBpirEntryKind::Event:
        return Node->IsA<UK2Node_Event>();

    default:
        return false;
    }
}

static UEdGraphNode* FindEntryNodeForBlock(
    const FBpirEntryBlock& Block,
    UBlueprint* Blueprint,
    UEdGraph* GraphContext,
    UEdGraphPin* EntryExecPin)
{
    UEdGraphNode* OwnerNode = EntryExecPin ? EntryExecPin->GetOwningNode() : nullptr;
    if (IsEntryNodeForBlock(Block, OwnerNode))
    {
        return OwnerNode;
    }

    TArray<UEdGraph*> Graphs;
    if (GraphContext)
    {
        Graphs.Add(GraphContext);
    }
    if (Blueprint)
    {
        Graphs.Append(Blueprint->UbergraphPages);
        Graphs.Append(Blueprint->FunctionGraphs);
        Graphs.Append(Blueprint->MacroGraphs);
    }

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (IsEntryNodeForBlock(Block, Node))
            {
                return Node;
            }
        }
    }

    return OwnerNode;
}

static void RejectImplicitVisibleHelpersForAuthoredBlock(
    const FBpirEntryBlock& Block,
    const FAuthoredPlacementMode& PlacementMode,
    const TArray<FGuid>& CreatedGUIDs,
    int32 CreatedStartIndex,
    UBlueprint* Blueprint,
    TArray<FCompileError>& Errors)
{
    if (!PlacementMode.bEnabled || !Blueprint)
    {
        return;
    }

    const int32 FirstSourceLine = [&Block]() -> int32
    {
        for (const FBpirInstruction& Inst : Block.Instructions)
        {
            if (Inst.bHasAuthoredPosition)
            {
                return Inst.SourceLine;
            }
        }
        return -1;
    }();

    for (int32 i = CreatedStartIndex; i < CreatedGUIDs.Num(); ++i)
    {
        const FGuid& Guid = CreatedGUIDs[i];
        if (PlacementMode.PrimaryNodeGuids.Contains(Guid))
        {
            continue;
        }

        UEdGraphNode* Node = FBlueprintEditorUtils::GetNodeByGUID(Blueprint, Guid);
        if (!Node || !Node->GetGraph())
        {
            continue;
        }

        // Pure expression nodes (K2Node_Self, pure K2Node_VariableGet, pure
        // function calls, knots) are never authored as standalone BPIR
        // instructions — they are inlined operands the compiler synthesises
        // automatically (e.g. `cast<Actor>(self)` materialises a K2Node_Self,
        // `$Speed` materialises a K2Node_VariableGet). Layout positions them
        // relative to their consumer; they have no @(x, y) of their own and
        // never need one.
        if (UK2Node* K2 = Cast<UK2Node>(Node))
        {
            if (K2->IsNodePure())
            {
                continue;
            }
        }

        const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        Errors.Add(FCompileError(
            FirstSourceLine,
            FString::Printf(
                TEXT("Manual BPIR placement error in entry body '%s' at source line %d: authored-position mode forbids implicit visible helper/generated node '%s' (%s) because it has no positioned BPIR instruction. %s"),
                *GetBpirEntryBodyName(Block),
                FirstSourceLine,
                *NodeTitle,
                *Node->GetClass()->GetName(),
                *GetBpirManualPlacementRemediation())));
    }
}

static TArray<FGuid> ExcludeAuthoredPlacementGuids(const TArray<FGuid>& CreatedGUIDs, const TSet<FGuid>& AuthoredPlacementGuids)
{
    if (AuthoredPlacementGuids.Num() == 0)
    {
        return CreatedGUIDs;
    }

    TArray<FGuid> LayoutGUIDs;
    LayoutGUIDs.Reserve(CreatedGUIDs.Num());
    for (const FGuid& Guid : CreatedGUIDs)
    {
        if (!AuthoredPlacementGuids.Contains(Guid))
        {
            LayoutGUIDs.Add(Guid);
        }
    }
    return LayoutGUIDs;
}

// ----------------------------------------------------------------------------
// Static Pin-Class Helper
// ----------------------------------------------------------------------------

UClass* FBpirCompiler::GetAuthoritativePinClass(UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return nullptr;
    }

    // Base class from the pin's type metadata (may be stale after ReconstructNode).
    UClass* BaseClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());

    // When the owning node is a ConstructObjectFromClass subclass (CreateWidget, SpawnActor, etc.),
    // GetClassToSpawn() reads ClassPin->DefaultObject — which is the authoritative spawn class set
    // during ExpandNode and stays correct even when pin subcategory specialization doesn't propagate.
    if (UK2Node_ConstructObjectFromClass* ConstructNode =
            Cast<UK2Node_ConstructObjectFromClass>(Pin->GetOwningNodeUnchecked()))
    {
        UClass* SpawnClass = ConstructNode->GetClassToSpawn();
        if (SpawnClass && SpawnClass != BaseClass
            && (BaseClass == nullptr || SpawnClass->IsChildOf(BaseClass)))
        {
            return SpawnClass;
        }

        // GetClassToSpawn() reads only DefaultObject; also try DefaultValue for nodes
        // that were wired with the string form only.
        UEdGraphPin* ClassPin = ConstructNode->GetClassPin();
        if (ClassPin && !ClassPin->DefaultValue.IsEmpty())
        {
            UClass* Resolved = ResolveUClass(ClassPin->DefaultValue);
            if (Resolved && (BaseClass == nullptr || Resolved->IsChildOf(BaseClass)))
            {
                return Resolved;
            }
        }
    }

    return BaseClass;
}

// ----------------------------------------------------------------------------
// Static Undo System
// ----------------------------------------------------------------------------

TArray<FGuid> FBpirCompiler::PopLastCreatedNodes()
{
    TArray<TArray<FGuid>>& Stack = FPluginState::Get().NodeCreationStack();
    TArray<bool>& Phase0Stack = FPluginState::Get().Phase0RanStack();
    if (Stack.Num() == 0)
    {
        return TArray<FGuid>();
    }
    TArray<FGuid> Last = Stack.Last();
    Stack.RemoveAt(Stack.Num() - 1);
    // Pop the parallel Phase0RanStack entry in lock-step. Guarded against drift in
    // case an earlier code path pushed only NodeCreationStack.
    if (Phase0Stack.Num() > 0)
    {
        Phase0Stack.RemoveAt(Phase0Stack.Num() - 1);
    }
    return Last;
}

bool FBpirCompiler::DidLastCompileRunPhase0()
{
    const TArray<bool>& Phase0Stack = FPluginState::Get().Phase0RanStack();
    if (Phase0Stack.Num() == 0)
    {
        return false;
    }
    return Phase0Stack.Last();
}

FEmittedNodeInfo& FBpirCompiler::GetOrCreateEmitInfo(int32 InstructionIndex)
{
    return EmitMap.FindOrAdd(InstructionIndex);
}

UClass* FBpirCompiler::ResolveTargetClass(const FString& TargetRef, FBpirEntryBlock& Block)
{
    // "self" → use the blueprint's own generated class directly.
    // UK2Node_Self's output pin uses PSC_Self subcategory with no PinSubCategoryObject,
    // so we can't extract a UClass from it.
    if (TargetRef == TEXT("self"))
    {
        return TargetBlueprint->GeneratedClass;
    }

    // Delegate to ValueResolver which handles all reference formats:
    // $varName, %refName, %refName.PinName, and chained property access
    UEdGraphPin* TargetPin = ValueResolver->ResolveValue(TargetRef, Block);

    if (!TargetPin)
    {
        UE_LOG(LogBpirCompiler, Error, TEXT("ResolveTargetClass: could not resolve target '%s'"), *TargetRef);
        return nullptr;
    }

    UClass* TargetClass = GetAuthoritativePinClass(TargetPin);
    if (!TargetClass)
    {
        UE_LOG(LogBpirCompiler, Error, TEXT("ResolveTargetClass: target '%s' pin type is not an object class"), *TargetRef);
    }
    return TargetClass;
}

// Handles the case where a caller's UK2Node_CallFunction reads from a skeleton
// UFunction whose return pin is missing or whose PinSubCategoryObject hasn't
// propagated the declared object<T> class yet, so downstream `Target: %var`
// resolution fails. Only affects BPIR-declared functions in the same compile.
//
// The lookup key is the BPIR call-site function name, which exactly matches
// what SetupFunction used to populate BpirDeclaredReturnTypes. Using the
// resolved UFunction's FName was unreliable because the skeleton UFunction
// can be resolved via fallback paths where GetFName() doesn't round-trip
// back to the BPIR entry name (e.g. when FName case or alias resolution
// introduces a mismatch).
UEdGraphPin* FBpirCompiler::EnsureCallReturnPinFromBpirDeclaration(
    UK2Node_CallFunction* CallNode,
    UEdGraphPin* ReturnPin,
    const FString& BpirFunctionName)
{
    if (!CallNode || BpirFunctionName.IsEmpty()) return ReturnPin;

    const FBpirTypeSpec* DeclaredRet = BpirDeclaredReturnTypes.Find(FName(*BpirFunctionName));
    if (!DeclaredRet || DeclaredRet->IsEmpty() || DeclaredRet->IsVoid()) return ReturnPin;

    FEdGraphPinType PatchedType;
    PatchedType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
    if (FCodePinResolver::ConvertTypeSpecToPinType(*DeclaredRet, PatchedType))
    {
        if (!ReturnPin)
        {
            ReturnPin = CallNode->CreatePin(EGPD_Output, PatchedType, UEdGraphSchema_K2::PN_ReturnValue);
        }
        if (!ReturnPin) return nullptr;
        ReturnPin->PinType = PatchedType;
    }
    return ReturnPin;
}

bool FBpirCompiler::DeleteNodesByGUIDs(UBlueprint* Blueprint, const TArray<FGuid>& NodeGUIDs)
{
    if (!Blueprint || NodeGUIDs.Num() == 0)
    {
        return false;
    }

    bool bAnyDeleted = false;
    for (const FGuid& Guid : NodeGUIDs)
    {
        UEdGraphNode* Node = FBlueprintEditorUtils::GetNodeByGUID(Blueprint, Guid);
        if (Node)
        {
            UEdGraph* Graph = Node->GetGraph();
            if (Graph)
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
                bAnyDeleted = true;
            }
        }
    }

    if (bAnyDeleted)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    return bAnyDeleted;
}

// ----------------------------------------------------------------------------
// Compile() — main entry point
// ----------------------------------------------------------------------------

// Forward decl — definition lives near the end of the file; referenced by the
// extend-reconnect pass in Compile() which sits above the definition.
static UEdGraphPin* FindTerminalExecOutputPin(UEdGraphNode* EntryNode);

// Walk exec-output edges from the queued roots and add every reachable node.
static void CollectDownstreamExecNodes(TArray<UEdGraphNode*> Queue, TSet<UEdGraphNode*>& Visited)
{
    while (Queue.Num() > 0)
    {
        UEdGraphNode* Current = Queue.Pop();
        if (!Current || Visited.Contains(Current)) continue;
        Visited.Add(Current);

        for (UEdGraphPin* Pin : Current->Pins)
        {
            if (!Pin) continue;
            if (Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin) continue;
                    if (UEdGraphNode* NextNode = LinkedPin->GetOwningNode())
                    {
                        if (!Visited.Contains(NextNode))
                        {
                            Queue.Add(NextNode);
                        }
                    }
                }
            }
        }
    }
}

static void CollectOwnedPureDependencies(TSet<UEdGraphNode*>& Visited)
{
    bool bAddedOwnedPureNode = true;
    while (bAddedOwnedPureNode)
    {
        bAddedOwnedPureNode = false;
        TArray<UEdGraphNode*> Snapshot = Visited.Array();

        for (UEdGraphNode* Node : Snapshot)
        {
            if (!Node)
            {
                continue;
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    UEdGraphNode* SourceNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
                    if (!SourceNode || Visited.Contains(SourceNode) || NodeHasExecPins(SourceNode))
                    {
                        continue;
                    }

                    bool bSharedOutsideDeletionSet = false;
                    for (UEdGraphPin* SourcePin : SourceNode->Pins)
                    {
                        if (!SourcePin || SourcePin->Direction != EGPD_Output)
                        {
                            continue;
                        }
                        if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                        {
                            continue;
                        }

                        for (UEdGraphPin* ConsumerPin : SourcePin->LinkedTo)
                        {
                            UEdGraphNode* ConsumerNode = ConsumerPin ? ConsumerPin->GetOwningNode() : nullptr;
                            if (ConsumerNode && !Visited.Contains(ConsumerNode))
                            {
                                bSharedOutsideDeletionSet = true;
                                break;
                            }
                        }

                        if (bSharedOutsideDeletionSet)
                        {
                            break;
                        }
                    }

                    if (!bSharedOutsideDeletionSet)
                    {
                        Visited.Add(SourceNode);
                        bAddedOwnedPureNode = true;
                    }
                }
            }
        }
    }
}

// Walk the exec-output graph starting from EntryNode and collect every reachable
// node. Then pull in upstream pure dependencies that are only consumed by the
// collected subgraph so replace mode does not leave stale owned data nodes behind.
static TSet<UEdGraphNode*> CollectSubgraphNodes(UEdGraphNode* EntryNode)
{
    TSet<UEdGraphNode*> Visited;
    if (!EntryNode) return Visited;

    TArray<UEdGraphNode*> Queue;
    Queue.Add(EntryNode);
    CollectDownstreamExecNodes(Queue, Visited);
    CollectOwnedPureDependencies(Visited);

    return Visited;
}

static TSet<UEdGraphNode*> CollectSubgraphNodesFromExecPin(UEdGraphPin* ExecPin)
{
    TSet<UEdGraphNode*> Visited;
    if (!ExecPin) return Visited;

    TArray<UEdGraphNode*> Queue;
    for (UEdGraphPin* LinkedPin : ExecPin->LinkedTo)
    {
        if (UEdGraphNode* NextNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr)
        {
            Queue.Add(NextNode);
        }
    }
    CollectDownstreamExecNodes(Queue, Visited);
    CollectOwnedPureDependencies(Visited);
    return Visited;
}

static TSet<UEdGraphNode*> CollectInputKeySubgraphNodesForEntry(UK2Node_InputKey* InputKeyNode, const bool bReleased)
{
    if (!InputKeyNode)
    {
        return TSet<UEdGraphNode*>();
    }

    if (FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, !bReleased))
    {
        return CollectSubgraphNodesFromExecPin(
            FBpirInputKeyHelpers::FindInputKeyExecPin(InputKeyNode, bReleased));
    }

    return CollectSubgraphNodes(InputKeyNode);
}

FCompileResult FBpirCompiler::Compile(const FString& Code, bool bReplaceMode)
{
    // Legacy bool overload: preserve the replace-mode semantics so existing call sites
    // (and tests using the bool API) route to the correct EBpirCompileMode variant.
    return Compile(Code, bReplaceMode ? EBpirCompileMode::Replace : EBpirCompileMode::Default);
}

FCompileResult FBpirCompiler::Compile(const FString& Code, EBpirCompileMode Mode)
{
    FString TrimmedCode = Code.TrimStartAndEnd();
    if (TrimmedCode.IsEmpty())
    {
        return FCompileResult::MakeError(-1, TEXT("Empty input code."));
    }

    // Reset per-compile state
    int32 ReplaceOrphansRemoved = 0;
    CompileMode = Mode;
    // bCompileReplaceMode gates replace-semantics in SetupCustomEvent (silently
    // replace an existing mismatched-signature event). Only Replace mode opts in;
    // Default mode preserves the stricter "already exists" error behavior so
    // signature conflicts surface to the caller instead of being silently fixed up.
    bCompileReplaceMode = (Mode == EBpirCompileMode::Replace);
    PendingExtendReconnects.Empty();
    PinResolver->Clear();
    NodeEmitter->GetCreatedNodeGUIDs().Empty();
    AccumulatedErrors.Empty();
    AccumulatedWarnings.Empty();
    CreatedFunctionGraphs.Empty();
    CreatedMacroGraphs.Empty();
    ReusedMacroGraphs.Empty();
    ReusedMacroGraphSnapshots.Empty();
    CreatedCustomEvents.Empty();
    BpirDeclaredReturnTypes.Reset();
    CurrentMacroExitTunnel = nullptr;
    ValueResolver->SetGraph(CurrentGraph);

    // Pass 1: Parse BPIR text into structured entry blocks
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> ParseErrors;
    FBpirParser Parser;

    if (!Parser.Parse(TrimmedCode, Blocks, ParseErrors))
    {
        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = ParseErrors;
        return ErrorResult;
    }

    TArray<FCompileError> PreflightErrors;
    if (!ValidateNoDuplicatePlainEntryEvents(Blocks, PreflightErrors))
    {
        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = PreflightErrors;
        return ErrorResult;
    }

    for (const FBpirEntryBlock& Block : Blocks)
    {
        if (Block.Kind != EBpirEntryKind::InputAction || Block.EntryExecTargets.IsEmpty())
        {
            continue;
        }

        for (const FBpirInstruction& Inst : Block.Instructions)
        {
            if (Inst.Opcode == EBpirOpcode::Label)
            {
                break;
            }
            if (Inst.Opcode != EBpirOpcode::Comment)
            {
                PreflightErrors.Add(FCompileError(Inst.SourceLine,
                    TEXT("input_action entries with an explicit event map cannot execute before the first label; put statements under a label")));
                break;
            }
        }
    }
    if (!PreflightErrors.IsEmpty())
    {
        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = PreflightErrors;
        return ErrorResult;
    }

    FScopedTransaction Transaction(NSLOCTEXT("BpirCompiler", "CompileCode", "Compile BPIR Code"));
    TargetBlueprint->Modify();

    // Pass 0: Force-load all externally-referenced UClass paths before any graph mutation.
    PreloadExternalClasses(Blocks);

    // Rollback contract: Phase 0-pre node deletions are NOT included in the
    // in-Compile atomic-rollback set (CreatedGUIDs at line ~1293). If Phase 1/2
    // fail, the rollback only deletes nodes created during this compile — Phase 0-pre
    // deletions stay deleted at the C++ level. Callers MUST wrap Compile() in an
    // FScopedTransaction and, on the failure path, finalize that scoped
    // transaction before rolling back through the normal editor undo path
    // (GEditor->UndoTransaction(false), exposed by TransactionUtils). Do not
    // call GUndo->Apply() while the scoped transaction is still active: UE graph
    // pin transaction serialization expects the editor transaction buffer to own
    // the active transaction state.
    // FBlueprintEditorUtils::RemoveNode (used by the delete loop below) calls
    // Modify() on graph + node before destruction, so the transactional snapshot
    // is captured automatically — the only requirement is caller-level
    // scoped-transaction finalization followed by editor undo.

    // Phase 0-pre (all modes): upsert ComponentEvent / WidgetEvent blocks.
    // UK2Node_ComponentBoundEvent nodes are structurally keyed on
    // {ComponentPropertyName, DelegatePropertyName} — emitting a second node
    // with the same pair produces two handlers firing on the same delegate, and
    // if the existing node carries references to a since-deleted class GUID
    // (e.g. a WidgetClass target that was asset_delete'd + asset_duplicate'd)
    // its downstream chain breaks BP compile. Neither Default nor Replace mode
    // can safely leave the old node in place, so match by name pair only and
    // delete before Phase 1 emits the new one. This runs ahead of the
    // Replace-mode scan below to avoid double-processing.
    {
        TSet<UEdGraphNode*> BoundEventNodesToDelete;
        for (const FBpirEntryBlock& Block : Blocks)
        {
            if (Block.Kind != EBpirEntryKind::ComponentEvent
                && Block.Kind != EBpirEntryKind::WidgetEvent)
            {
                continue;
            }
            if (Block.ComponentName.IsEmpty() || Block.Name.IsEmpty())
            {
                continue;
            }

            const FName CompKey(*Block.ComponentName);
            const FName EventKey(*Block.Name);
            const FString EventKeyNormalized = Block.Name.Replace(TEXT(" "), TEXT(""));

            for (UEdGraph* Graph : TargetBlueprint->UbergraphPages)
            {
                if (!Graph) continue;
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    UK2Node_ComponentBoundEvent* Existing = Cast<UK2Node_ComponentBoundEvent>(Node);
                    if (!Existing) continue;
                    if (Existing->ComponentPropertyName != CompKey) continue;

                    const FName ExistingDelegate = Existing->DelegatePropertyName;
                    const bool bMatchesRaw = (ExistingDelegate == EventKey);
                    const bool bMatchesCaseless =
                        ExistingDelegate.ToString().Replace(TEXT(" "), TEXT(""))
                            .Equals(EventKeyNormalized, ESearchCase::IgnoreCase);
                    if (bMatchesRaw || bMatchesCaseless)
                    {
                        BoundEventNodesToDelete.Append(CollectSubgraphNodes(Existing));
                    }
                }
            }
        }

        if (BoundEventNodesToDelete.Num() > 0)
        {
            for (UEdGraphNode* Node : BoundEventNodesToDelete)
            {
                if (!Node) continue;
                FBlueprintEditorUtils::RemoveNode(TargetBlueprint, Node, /*bDontRecompile=*/true);
            }
        }
    }

    // Phase 0 (replace-mode only): delete existing entry nodes named in the incoming BPIR,
    // along with their exec-reachable subgraphs. Pure-only data dependencies become
    // orphans that the skeleton recompile / next compile can prune.
    //
    // Gated on Replace mode (not Default) so that Default mode preserves existing entry
    // nodes — this is required by the contract that:
    //   * AtomicRollback: a failed Default compile must restore pre-compile node count.
    //     If Phase 0 deletes existing entries then the compile fails mid-way, the atomic
    //     rollback at the end of Compile() only removes nodes CREATED during the compile
    //     (tracked via NodeEmitter GUIDs); it cannot restore nodes deleted in Phase 0.
    //   * UndoCompile: PopLastCreatedNodes + DeleteNodesByGUIDs must return to baseline,
    //     which also relies on no Phase 0 deletions in Default mode.
    //   * CustomEventSignatureConflict (Default): re-compiling the same custom-event name
    //     with a mismatched signature must produce an "already exists" error, which
    //     SetupCustomEvent only raises when Phase 0 has NOT pre-deleted the old node.
    //
    // Skipped in Extend mode — extend walks the existing chain and splices after its terminal.
    //
    // NOTE: the prior unconditional variant was intended to prevent duplicate entry-node
    // accumulation on retry-after-timeout. That scenario should now be handled by callers
    // invoking Compile() with EBpirCompileMode::Replace explicitly, or by a future
    // transaction/snapshot mechanism that rolls back Phase 0 deletions on compile failure.
    // Track whether the Replace-mode Phase 0 deletion block runs. blueprint.undo_last_bpir
    // refuses to roll back when this is true — un-creating the compile's nodes cannot
    // restore the swept pre-existing entry subgraphs (see B-undo-last-bpir-doesnt-restore-phase0-sweeps).
    const bool bPhase0Ran = (Mode == EBpirCompileMode::Replace);
    if (bPhase0Ran)
    {
        TSet<UEdGraphNode*> NodesToDelete;
        TArray<UEdGraph*> FunctionGraphsToRemove;
        // Tracks UFunction names wiped in this pass so CascadeRemoveStaleCreateDelegates
        // can clean up UK2Node_CreateDelegate nodes that reference them from outside the
        // subgraph. These survive CollectSubgraphNodes (which only walks exec-output pins
        // and pure-upstream dependencies) and would otherwise orphan with stale
        // SelectedFunctionName references, crashing FKismetCompilerContext::
        // ReplaceConvertibleDelegates on cold reload.
        TSet<FName> WipedFunctionNames;

        for (const FBpirEntryBlock& Block : Blocks)
        {
            const FString& EntryName = Block.Name;
            if (EntryName.IsEmpty()) continue;

            if (Block.Kind == EBpirEntryKind::Event)
            {
                // Map shorthand name (e.g. "BeginPlay") to the real UFunction name
                // (e.g. "ReceiveBeginPlay") that K2Node_Event stores. Class-aware:
                // keep the literal name when the parent class exposes it directly
                // (e.g. UUserWidget::Tick) — see ResolveOverrideEventName.
                const FName ResolvedName = ResolveOverrideEventName(EntryName, TargetBlueprint ? TargetBlueprint->ParentClass : nullptr);
                const FName MappedName = (ResolvedName != FName(*EntryName)) ? ResolvedName : NAME_None;

                for (UEdGraph* Graph : TargetBlueprint->UbergraphPages)
                {
                    if (!Graph) continue;
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
                        if (!EventNode) continue;
                        // K2Node_CustomEvent derives from K2Node_Event — skip here,
                        // it's handled in the CustomEvent branch below.
                        if (EventNode->IsA<UK2Node_CustomEvent>()) continue;

                        const FName MemberName = EventNode->EventReference.GetMemberName();
                        const bool bMatchesMapped = (MappedName != NAME_None && MemberName == MappedName);
                        const bool bMatchesRaw = MemberName.ToString().Equals(EntryName, ESearchCase::IgnoreCase);
                        if (bMatchesMapped || bMatchesRaw)
                        {
                            NodesToDelete.Append(CollectSubgraphNodes(EventNode));
                            WipedFunctionNames.Add(MappedName != NAME_None ? MappedName : MemberName);
                        }
                    }
                }
            }
            else if (Block.Kind == EBpirEntryKind::CustomEvent
                  || Block.Kind == EBpirEntryKind::KeyPressed
                  || Block.Kind == EBpirEntryKind::KeyReleased
                  || Block.Kind == EBpirEntryKind::InputAction)
            {
                // ComponentEvent/WidgetEvent are owned by the Phase 0-pre pass above —
                // they use UK2Node_ComponentBoundEvent (not UK2Node_CustomEvent) and
                // need {ComponentName, DelegateName} keying regardless of mode.
                const bool bInputKeyEntry =
                    Block.Kind == EBpirEntryKind::KeyPressed
                    || Block.Kind == EBpirEntryKind::KeyReleased;
                const bool bInputKeyReleased = Block.Kind == EBpirEntryKind::KeyReleased;
                for (UEdGraph* Graph : TargetBlueprint->UbergraphPages)
                {
                    if (!Graph) continue;
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        if (Block.Kind == EBpirEntryKind::InputAction)
                        {
                            if (IsEntryNodeForBlock(Block, Node))
                            {
                                NodesToDelete.Append(CollectSubgraphNodes(Node));
                            }
                            continue;
                        }
                        if (bInputKeyEntry)
                        {
                            UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node);
                            if (FBpirInputKeyHelpers::IsInputKeyNodeForBpirEntry(
                                InputKeyNode,
                                EntryName,
                                bInputKeyReleased))
                            {
                                NodesToDelete.Append(
                                    CollectInputKeySubgraphNodesForEntry(InputKeyNode, bInputKeyReleased));
                            }
                            continue;
                        }

                        UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
                        if (!CE) continue;
                        if (CE->CustomFunctionName.ToString().Equals(EntryName, ESearchCase::IgnoreCase))
                        {
                            NodesToDelete.Append(CollectSubgraphNodes(CE));
                            WipedFunctionNames.Add(CE->CustomFunctionName);
                        }
                    }
                }
            }
            else if (Block.Kind == EBpirEntryKind::Override)
            {
                BlueprintHandlerUtils::FBlueprintOverrideInfo OverrideInfo;
                FString OverrideError;
                if (BlueprintHandlerUtils::TryResolveBlueprintOverride(
                    TargetBlueprint,
                    EntryName,
                    OverrideInfo,
                    OverrideError))
                {
                    if (OverrideInfo.bCanPlaceAsEvent)
                    {
                        for (UEdGraph* Graph : TargetBlueprint->UbergraphPages)
                        {
                            if (!Graph) continue;
                            for (UEdGraphNode* Node : Graph->Nodes)
                            {
                                UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
                                if (!EventNode || !EventNode->bOverrideFunction) continue;
                                if (EventNode->EventReference.GetMemberName().ToString().Equals(EntryName, ESearchCase::IgnoreCase))
                                {
                                    NodesToDelete.Append(CollectSubgraphNodes(EventNode));
                                    WipedFunctionNames.Add(EventNode->EventReference.GetMemberName());
                                }
                            }
                        }
                    }
                    else
                    {
                        bool bIsInterfaceOwned = false;
                        if (UEdGraph* ExistingGraph = BlueprintHandlerUtils::FindFunctionGraphByName(
                            TargetBlueprint,
                            EntryName,
                            &bIsInterfaceOwned))
                        {
                            if (bIsInterfaceOwned)
                            {
                                CollectFunctionBodyNodesPreservingTerminators(ExistingGraph, NodesToDelete);
                            }
                            else
                            {
                                FunctionGraphsToRemove.AddUnique(ExistingGraph);
                                WipedFunctionNames.Add(FName(*EntryName));
                            }
                        }
                    }
                }
            }
            else if (Block.Kind == EBpirEntryKind::Function
                  || Block.Kind == EBpirEntryKind::Macro)
            {
                // Functions/macros live in their own graph — find by graph name.
                if (Block.Kind == EBpirEntryKind::Macro)
                {
                    if (UEdGraph* ExistingMacroGraph = BpirCompilerMacroUtils::FindMacroGraphByName(TargetBlueprint, FName(*EntryName)))
                    {
                        ReusedMacroGraphs.AddUnique(ExistingMacroGraph);
                    }
                }
                else
                {
                    bool bIsInterfaceOwned = false;
                    UEdGraph* ExistingFunctionGraph = BlueprintHandlerUtils::FindFunctionGraphByName(
                        TargetBlueprint,
                        EntryName,
                        &bIsInterfaceOwned);
                    if (ExistingFunctionGraph && bIsInterfaceOwned)
                    {
                        CollectFunctionBodyNodesPreservingTerminators(ExistingFunctionGraph, NodesToDelete);
                    }
                    else
                    {
                        for (UEdGraph* Graph : TargetBlueprint->FunctionGraphs)
                        {
                            if (!Graph) continue;
                            if (Graph->GetFName().ToString().Equals(EntryName, ESearchCase::IgnoreCase))
                            {
                                FunctionGraphsToRemove.AddUnique(Graph);
                                WipedFunctionNames.Add(Graph->GetFName());
                            }
                        }
                    }

                    // For Function kind, also check if the function is an override placed
                    // as an event node (the auto-detect logic may route to SetupOverride which
                    // creates an event). Clean up the event to avoid duplicates.
                    BlueprintHandlerUtils::FBlueprintOverrideInfo FuncOverrideInfo;
                    FString FuncOverrideError;
                    if (BlueprintHandlerUtils::TryResolveBlueprintOverride(
                        TargetBlueprint, EntryName, FuncOverrideInfo, FuncOverrideError)
                        && FuncOverrideInfo.bCanPlaceAsEvent)
                    {
                        for (UEdGraph* Graph : TargetBlueprint->UbergraphPages)
                        {
                            if (!Graph) continue;
                            for (UEdGraphNode* Node : Graph->Nodes)
                            {
                                UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
                                if (!EventNode || !EventNode->bOverrideFunction) continue;
                                if (EventNode->EventReference.GetMemberName().ToString().Equals(
                                    EntryName, ESearchCase::IgnoreCase))
                                {
                                    NodesToDelete.Append(CollectSubgraphNodes(EventNode));
                                    WipedFunctionNames.Add(EventNode->EventReference.GetMemberName());
                                }
                            }
                        }
                    }
                }
            }
            else if (Block.Kind == EBpirEntryKind::Construction)
            {
                // ConstructionScript is a named function graph ("UserConstructionScript").
                // In replace mode, wipe its body but don't remove the graph itself.
                for (UEdGraph* Graph : TargetBlueprint->FunctionGraphs)
                {
                    if (!Graph) continue;
                    const FString GraphName = Graph->GetFName().ToString();
                    if (GraphName.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase)
                        || GraphName.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase))
                    {
                        for (UEdGraphNode* Node : Graph->Nodes)
                        {
                            UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
                            if (EntryNode)
                            {
                                NodesToDelete.Append(CollectSubgraphNodes(EntryNode));
                                // Don't delete the entry node itself — reuse it.
                                NodesToDelete.Remove(EntryNode);
                            }
                        }
                    }
                }
            }
        }

        // Collect the set of graphs affected by deletion (for orphan cleanup).
        TSet<UEdGraph*> AffectedGraphs;
        for (UEdGraphNode* Node : NodesToDelete)
        {
            if (Node && Node->GetGraph())
            {
                AffectedGraphs.Add(Node->GetGraph());
            }
        }

        for (UEdGraphNode* Node : NodesToDelete)
        {
            if (!Node) continue;
            FBlueprintEditorUtils::RemoveNode(TargetBlueprint, Node, /*bDontRecompile=*/true);
        }

        for (UEdGraph* Graph : FunctionGraphsToRemove)
        {
            FBlueprintEditorUtils::RemoveGraph(TargetBlueprint, Graph);
        }

        if (NodesToDelete.Num() > 0 || FunctionGraphsToRemove.Num() > 0)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(TargetBlueprint);
        }

        // Phase 0c: Remove UK2Node_CreateDelegate nodes that live OUTSIDE the wiped
        // subgraphs but reference one of the now-deleted entry UFunction names via
        // SelectedFunctionName. CollectSubgraphNodes does not traverse delegate pins,
        // so these nodes survive Phase 0 as orphans. On cold reload,
        // FKismetCompilerContext::ReplaceConvertibleDelegates crashes on them because
        // the referenced UFunction no longer exists.
        BlueprintHandlerUtils::CascadeRemoveStaleCreateDelegates(TargetBlueprint, WipedFunctionNames);

        // Scrub the backing UFunctions too — Phase 1.5's RegenerateSkeletonOnly won't.
        // Without this, orphan UFunctions from wiped entry nodes survive on the
        // generated class and crash cold reload. See B-bp-saved-state-corruption-mcp-edits P0-10.
        BlueprintHandlerUtils::ScrubStaleUFunctionsFromClass(TargetBlueprint, WipedFunctionNames);

        // Phase 0b: Clean up orphaned pure nodes left behind by subgraph deletion.
        // Fixed-point loop handles chains (deleting one pure node may orphan the next).
        if (AffectedGraphs.Num() > 0)
        {
            int32 SafetyBound = 0;
            for (UEdGraph* G : AffectedGraphs) { SafetyBound += G ? G->Nodes.Num() : 0; }

            bool bFoundOrphans = true;
            while (bFoundOrphans && SafetyBound-- > 0)
            {
                bFoundOrphans = false;
                for (UEdGraph* Graph : AffectedGraphs)
                {
                    if (!Graph) continue;
                    TArray<UEdGraphNode*> PureOrphans;
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        if (!Node) continue;
                        // NodeHasExecPins catches all entry nodes too (they all have exec outputs),
                        // so the IsA checks below are purely defensive.
                        if (NodeHasExecPins(Node)) continue;
                        if (Node->IsA<UEdGraphNode_Comment>()) continue;
                        if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>()
                            || Node->IsA<UK2Node_FunctionEntry>()
                            || Node->IsA<UK2Node_InputKey>()
                            || Node->IsA<UK2Node_ComponentBoundEvent>()) continue;

                        bool bAllOutputsDisconnected = true;
                        bool bHasOutputs = false;
                        for (const UEdGraphPin* Pin : Node->Pins)
                        {
                            if (Pin && Pin->Direction == EGPD_Output)
                            {
                                bHasOutputs = true;
                                if (Pin->LinkedTo.Num() > 0)
                                {
                                    bAllOutputsDisconnected = false;
                                    break;
                                }
                            }
                        }
                        if (bHasOutputs && bAllOutputsDisconnected)
                        {
                            PureOrphans.Add(Node);
                        }
                    }
                    for (UEdGraphNode* Orphan : PureOrphans)
                    {
                        FBlueprintEditorUtils::RemoveNode(TargetBlueprint, Orphan, /*bDontRecompile=*/true);
                        ReplaceOrphansRemoved++;
                        bFoundOrphans = true;
                    }
                }
            }
        }
    }

    // Phase 0c: Ensure SkeletonGeneratedClass exists before override detection.
    // Widget Blueprints or freshly-created BPs may not have a skeleton yet.
    // SetupOverride / TryResolveBlueprintOverride need it for GetOverrideFunctionClass.
    if (!TargetBlueprint->SkeletonGeneratedClass)
    {
        FKismetEditorUtilities::CompileBlueprint(TargetBlueprint, EBlueprintCompileOptions::RegenerateSkeletonOnly);
    }

    // Phase 1: Create all entry points so skeleton recompile can expose custom events as UFunctions
    struct FBlockSetupState
    {
        UEdGraphPin* EntryExecPin = nullptr;
        UEdGraph* GraphContext = nullptr;
        UEdGraphNode* EntryNode = nullptr; // Cached for pin re-registration after PinResolver clear.
                                          // Safe across skeleton recompile: RegenerateSkeletonOnly patches
                                          // existing nodes in-place rather than destroying/recreating them.
        // Entry node whose position the author specified via `entry ... @(x, y)`. Tracked
        // separately because it must be excluded from the layout pass — otherwise the
        // collision-resolver shifts it to make room for body nodes and the @(x, y)
        // suffix no longer round-trips.
        UEdGraphNode* AuthoredEntryNode = nullptr;
        UK2Node_Tunnel* MacroExitTunnel = nullptr;
        bool bValid = false;
    };
    TArray<FBlockSetupState> BlockStates;
    BlockStates.SetNum(Blocks.Num());

    for (int32 BlockIdx = 0; BlockIdx < Blocks.Num(); ++BlockIdx)
    {
        FBpirEntryBlock& Block = Blocks[BlockIdx];

        // Reset graph context to event graph for non-function entry kinds,
        // since SetupFunction/SetupConstructionScript switch CurrentGraph
        if (Block.Kind == EBpirEntryKind::Event
            || Block.Kind == EBpirEntryKind::CustomEvent
            || Block.Kind == EBpirEntryKind::ComponentEvent
            || Block.Kind == EBpirEntryKind::WidgetEvent
            || Block.Kind == EBpirEntryKind::KeyPressed
            || Block.Kind == EBpirEntryKind::KeyReleased
            || Block.Kind == EBpirEntryKind::InputAction)
        {
            if (TargetBlueprint->UbergraphPages.Num() == 0)
            {
                AccumulatedErrors.Add(FCompileError{-1, TEXT("Blueprint has no event graph (UbergraphPages is empty)")});
                continue;
            }
            SwitchToGraph(TargetBlueprint->UbergraphPages[0]);
        }

        const int32 ErrorsBeforeSetup = AccumulatedErrors.Num();
        UEdGraphPin* EntryExecPin = SetupEntryPoint(Block);
        if (!EntryExecPin && Block.Kind != EBpirEntryKind::Macro)
        {
            UE_LOG(LogBpirCompiler, Warning, TEXT("Failed to create entry point for block: %s"), *Block.Name);
            if (AccumulatedErrors.Num() == ErrorsBeforeSetup)
            {
                AccumulatedErrors.Add(FCompileError{-1, FString::Printf(TEXT("Failed to create entry point for block: %s"), *Block.Name)});
            }
            continue;
        }

        // Find the entry node that owns the exec pin (for pin re-registration in Phase 2)
        UEdGraphNode* EntryNode = EntryExecPin ? EntryExecPin->GetOwningNode() : nullptr;
        UEdGraphNode* AuthoredEntryNode = FindEntryNodeForBlock(Block, TargetBlueprint, CurrentGraph, EntryExecPin);
        ApplyAuthoredEntryPositionForBlock(Block, AuthoredEntryNode);
        ApplyBpirEnabledState(Block.EnabledState, Block.bHasEnabledState, AuthoredEntryNode);

        BlockStates[BlockIdx].EntryExecPin = EntryExecPin;
        BlockStates[BlockIdx].GraphContext = CurrentGraph;
        BlockStates[BlockIdx].EntryNode = EntryNode ? EntryNode : AuthoredEntryNode;
        BlockStates[BlockIdx].AuthoredEntryNode = Block.bHasAuthoredEntryPosition ? AuthoredEntryNode : nullptr;
        BlockStates[BlockIdx].MacroExitTunnel = CurrentMacroExitTunnel;
        BlockStates[BlockIdx].bValid = true;
    }

    // Phase 1.5: skeleton recompile when any Phase-1 output lacks a UFunction on
    // SkeletonGeneratedClass — either a newly-created custom event, or a user function
    // graph that hasn't been skeleton-compiled yet. Keeps resolver lookups up-to-date.
    auto NeedsSkeletonRecompile = [this]() -> bool
    {
        if (CreatedCustomEvents.Num() > 0) return true;
        UClass* const SGC = TargetBlueprint->SkeletonGeneratedClass;
        for (UEdGraph* FG : TargetBlueprint->FunctionGraphs)
        {
            if (!FG) continue;
            if (!SGC || !SGC->FindFunctionByName(FG->GetFName())) return true;
        }
        return false;
    };
    if (NeedsSkeletonRecompile())
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(TargetBlueprint);
        FKismetEditorUtilities::CompileBlueprint(TargetBlueprint, EBlueprintCompileOptions::RegenerateSkeletonOnly);
    }

    TSet<FGuid> AuthoredPlacementGuids;

    // Phase 2: Emit and wire instructions for each block
    for (int32 BlockIdx = 0; BlockIdx < Blocks.Num(); ++BlockIdx)
    {
        FBpirEntryBlock& Block = Blocks[BlockIdx];
        if (!BlockStates[BlockIdx].bValid) continue;

        // Reset per-block state
        EmitMap.Empty();
        PinResolver->Clear();
        SwitchToGraph(BlockStates[BlockIdx].GraphContext);
        ValueResolver->SetEmitMap(&EmitMap);
        CurrentMacroExitTunnel = BlockStates[BlockIdx].MacroExitTunnel;

        UEdGraphPin* EntryExecPin = BlockStates[BlockIdx].EntryExecPin;

        // Re-register entry point parameter pins (PinResolver was cleared between phases).
        // Generically re-register ALL non-exec output pins on the entry node — this covers
        // custom event params, function params, builtin event params (DeltaSeconds, OtherActor),
        // component event params, and macro input params.
        if (UEdGraphNode* CachedEntryNode = BlockStates[BlockIdx].EntryNode)
        {
            ApplyBpirEnabledState(Block.EnabledState, Block.bHasEnabledState, CachedEntryNode);

            for (UEdGraphPin* Pin : CachedEntryNode->Pins)
            {
                if (Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                    && !Pin->PinName.IsNone())
                {
                    PinResolver->RegisterVariable(Pin->PinName.ToString(), Pin);
                }
            }

            // Author-declared param aliases were registered in Phase 1 but wiped by
            // PinResolver->Clear(); re-apply them so PreEmitVariableRefs can resolve
            // aliases that differ from the UE delegate's canonical pin name.
            if (Block.Kind == EBpirEntryKind::WidgetEvent
                || Block.Kind == EBpirEntryKind::ComponentEvent
                || Block.Kind == EBpirEntryKind::CustomEvent)
            {
                RegisterEntryParamAliases(CachedEntryNode, Block.Params);
            }
        }

        // SwitchToGraph reset the emitter's placement cursor to (0, 0). Re-anchor
        // it to this block's entry node so PreEmitVariableRefs and EmitInstruction
        // place body nodes near the entry's Y rather than at the top of the graph.
        // ResetPlacementForChain is a no-op when the pin is null (e.g. pure macros).
        NodeEmitter->ResetPlacementForChain(EntryExecPin, 0);

        const int32 BlockCreatedStartIndex = NodeEmitter->GetCreatedNodeGUIDs().Num();

        // Pass 2b: Pre-emit VariableGet/Self nodes for all $var and self references
        PreEmitVariableRefs(Block);

        // Pass 2c: Emit K2 nodes for each instruction
        UEdGraphPin* CurrentExecPin = Block.EntryExecTargets.IsEmpty() ? EntryExecPin : nullptr;
        for (int32 i = 0; i < Block.Instructions.Num(); ++i)
        {
            FBpirInstruction& Inst = Block.Instructions[i];
            if (Inst.Opcode == EBpirOpcode::Label)
            {
                CurrentExecPin = nullptr;
                continue;
            }
            if (Inst.Opcode == EBpirOpcode::Comment)
            {
                continue;
            }
            if (Inst.Opcode == EBpirOpcode::ExecGoto || Inst.Opcode == EBpirOpcode::End)
            {
                CurrentExecPin = nullptr;
                continue;
            }

            if (!EmitInstruction(i, Inst, Block, CurrentExecPin))
            {
                UE_LOG(LogBpirCompiler, Warning, TEXT("EmitInstruction failed for opcode %d at line %d"),
                    static_cast<int32>(Inst.Opcode), Inst.SourceLine);
                AccumulatedErrors.Add(FCompileError{Inst.SourceLine,
                    FString::Printf(TEXT("EmitInstruction failed for opcode %d"), static_cast<int32>(Inst.Opcode))});
            }

            if (const FEmittedNodeInfo* Emitted = EmitMap.Find(i))
            {
                ApplyBpirEnabledState(Inst.EnabledState, Inst.bHasEnabledState, Emitted->Node);
            }

            // Multi-output nodes manage their own exec wiring via labels
            if (Inst.HasExecTargets())
            {
                CurrentExecPin = nullptr;
            }
        }

        FAuthoredPlacementMode PlacementMode = ApplyAuthoredPositionsForBlock(Block, EmitMap, AccumulatedErrors);
        for (const FGuid& Guid : PlacementMode.PrimaryNodeGuids)
        {
            AuthoredPlacementGuids.Add(Guid);
        }

        // Entry node carries its own `entry ... @(x, y)` position separate from the
        // body's per-instruction positions. Without this exclusion the layout pass
        // shifts the entry node to deconflict with body clusters and the suffix
        // drifts (a +2 push was the original symptom).
        if (UEdGraphNode* AuthoredEntry = BlockStates[BlockIdx].AuthoredEntryNode)
        {
            AuthoredPlacementGuids.Add(AuthoredEntry->NodeGuid);
        }

        // Pass 3a: Wire data pins for each instruction that has arguments
        for (int32 i = 0; i < Block.Instructions.Num(); ++i)
        {
            FBpirInstruction& Inst = Block.Instructions[i];
            const FEmittedNodeInfo* Info = EmitMap.Find(i);
            if (Inst.Args.Num() > 0 && Info && Info->Node && !Info->bSkipWireDataPins)
            {
                if (!WireDataPins(i, Inst, Block))
                {
                    // Errors already added to AccumulatedErrors by WireDataPins
                }
            }
        }

        // Pass 3b: Wire exec pins (auto-chain within label blocks + explicit label targets)
        if (!WireExecPins(Block, EntryExecPin))
        {
            // Errors already added to AccumulatedErrors by WireExecPins
        }

        RejectImplicitVisibleHelpersForAuthoredBlock(
            Block,
            PlacementMode,
            NodeEmitter->GetCreatedNodeGUIDs(),
            BlockCreatedStartIndex,
            TargetBlueprint,
            AccumulatedErrors);
    }

    // Extend mode post-wiring: reconnect downstream pins that were displaced by the splice.
    // When SetupOverride found a TerminalPin already linking to a FunctionResult (return) node,
    // TryCreateConnection broke that link to wire in the new body's first exec-in. We restore it
    // by connecting the new body's last exec-output to the saved downstream pins.
    for (FExtendReconnect& Reconnect : PendingExtendReconnects)
    {
        if (!Reconnect.SplicePin || Reconnect.DownstreamPins.Num() == 0) continue;

        // Find the new body's last exec-output: walk forward from SplicePin through any newly
        // created nodes to the last one with no outgoing exec links.
        UEdGraphPin* LastNewExecOut = FindTerminalExecOutputPin(Reconnect.SplicePin->GetOwningNode());
        if (!LastNewExecOut) LastNewExecOut = Reconnect.SplicePin;

        // Reconnect each downstream pin (typically just the FunctionResult exec-in).
        const UEdGraphSchema* Schema = LastNewExecOut->GetSchema();
        if (Schema)
        {
            for (UEdGraphPin* DsPin : Reconnect.DownstreamPins)
            {
                if (!DsPin) continue;
                if (!Schema->TryCreateConnection(LastNewExecOut, DsPin))
                {
                    UE_LOG(LogBpirCompiler, Warning,
                        TEXT("Extend mode: failed to reconnect downstream exec '%s'"), *DsPin->PinName.ToString());
                }
            }
        }
    }
    PendingExtendReconnects.Empty();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(TargetBlueprint);

    TArray<FGuid> CreatedGUIDs = NodeEmitter->GetCreatedNodeGUIDs();
    auto RollbackCreatedState = [&]()
    {
        DeleteNodesByGUIDs(TargetBlueprint, CreatedGUIDs);
        for (UEdGraph* Graph : CreatedFunctionGraphs)
        {
            FBlueprintEditorUtils::RemoveGraph(TargetBlueprint, Graph);
        }
        CreatedFunctionGraphs.Empty();
        for (UEdGraph* Graph : CreatedMacroGraphs)
        {
            FBlueprintEditorUtils::RemoveGraph(TargetBlueprint, Graph);
        }
        CreatedMacroGraphs.Empty();
        RestoreReusedMacroGraphSnapshots(TargetBlueprint, ReusedMacroGraphSnapshots);
        ReusedMacroGraphSnapshots.Empty();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(TargetBlueprint);
    };

    // Atomic rollback: if any errors accumulated, delete all created nodes and graphs
    if (AccumulatedErrors.Num() > 0)
    {
        RollbackCreatedState();

        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = AccumulatedErrors;
        return ErrorResult;
    }

    if (ReusedMacroGraphs.Num() > 0)
    {
        TSet<UEdGraph*> ReusedMacroGraphSet;
        for (UEdGraph* Graph : ReusedMacroGraphs)
        {
            if (Graph)
            {
                ReusedMacroGraphSet.Add(Graph);
            }
        }

        TSet<FGuid> CreatedNodeGuidSet;
        for (const FGuid& Guid : CreatedGUIDs)
        {
            CreatedNodeGuidSet.Add(Guid);
        }

        TArray<FBpirMacroCallerSnapshot> MacroCallerSnapshots;
        if (!ReconstructSameBlueprintMacroCallers(
            TargetBlueprint,
            ReusedMacroGraphSet,
            CreatedNodeGuidSet,
            AccumulatedErrors,
            MacroCallerSnapshots))
        {
            RollbackCreatedState();
            ReconstructMacroCallersFromSnapshots(MacroCallerSnapshots);
            RestoreMacroCallerSnapshots(MacroCallerSnapshots);

            FCompileResult ErrorResult;
            ErrorResult.bSuccess = false;
            ErrorResult.Errors = AccumulatedErrors;
            return ErrorResult;
        }
    }

    // --- Layout pass ---
    {
        TArray<UEdGraph*> ExtraGraphs;
        ExtraGraphs.Append(CreatedFunctionGraphs);
        ExtraGraphs.Append(CreatedMacroGraphs);
        ExtraGraphs.Append(ReusedMacroGraphs);
        for (const FBlockSetupState& BlockState : BlockStates)
        {
            if (BlockState.bValid && BlockState.GraphContext)
            {
                // Interface-owned function graphs are reused, so they must participate in
                // layout without being added to the rollback-owned CreatedFunctionGraphs.
                ExtraGraphs.Add(BlockState.GraphContext);
            }
        }
        RunLayoutPass(TargetBlueprint, ExcludeAuthoredPlacementGuids(CreatedGUIDs, AuthoredPlacementGuids), CurrentGraph, ExtraGraphs);
    }

    // Safety-net: scan ubergraph for duplicate entry nodes sharing a signature.
    // These indicate remnants of a prior partial write that survived Phase 0 cleanup
    // (e.g. a node that was added by an earlier compile but with a slightly different
    // internal state). Report per-signature counts as warnings so callers can detect
    // and investigate if they appear.
    {
        TMap<FString, int32> CustomEventCounts;
        TMap<FString, int32> InputKeyCounts;
        for (UEdGraph* Graph : TargetBlueprint->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
                {
                    const FString Sig = CE->CustomFunctionName.ToString();
                    CustomEventCounts.FindOrAdd(Sig)++;
                    continue;
                }

                if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node))
                {
                    const FString KeyIdentifier =
                        FBpirInputKeyHelpers::FormatInputKeyAsBpirIdentifier(InputKeyNode->InputKey);
                    if (KeyIdentifier.IsEmpty())
                    {
                        continue;
                    }
                    if (FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, false))
                    {
                        InputKeyCounts.FindOrAdd(
                            FString::Printf(TEXT("key_pressed %s"), *KeyIdentifier))++;
                    }
                    if (FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, true))
                    {
                        InputKeyCounts.FindOrAdd(
                            FString::Printf(TEXT("key_released %s"), *KeyIdentifier))++;
                    }
                }
            }
        }
        for (const auto& KV : CustomEventCounts)
        {
            if (KV.Value > 1)
            {
                AccumulatedWarnings.Add(FString::Printf(
                    TEXT("Duplicate entry nodes detected for signature '%s' — possible remnants of prior partial writes"),
                    *KV.Key));
            }
        }
        for (const auto& KV : InputKeyCounts)
        {
            if (KV.Value > 1)
            {
                AccumulatedWarnings.Add(FString::Printf(
                    TEXT("Duplicate input-key entry nodes detected for signature '%s' - possible remnants of prior partial writes"),
                    *KV.Key));
            }
        }
    }

    FPluginState::Get().NodeCreationStack().Add(CreatedGUIDs);
    FPluginState::Get().Phase0RanStack().Add(bPhase0Ran);
    FCompileResult SuccessResult = FCompileResult::MakeSuccess(CreatedGUIDs);
    SuccessResult.Warnings = AccumulatedWarnings;
    SuccessResult.OrphansRemoved = ReplaceOrphansRemoved;
    return SuccessResult;
}

// ----------------------------------------------------------------------------
// InsertCodeAfterNode()
// ----------------------------------------------------------------------------

FCompileResult FBpirCompiler::InsertCodeAfterNode(UEdGraphNode* InsertionPointNode, const FString& Code, const FString& ExecPinName)
{
    if (!InsertionPointNode)
    {
        return FCompileResult::MakeError(-1, TEXT("InsertCodeAfterNode: null insertion point node."));
    }

    // Find the exec output pin of the insertion node
    UEdGraphPin* ExecOutPin = nullptr;
    if (ExecPinName.IsEmpty())
    {
        // Default behavior: first exec output pin
        for (UEdGraphPin* Pin : InsertionPointNode->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                ExecOutPin = Pin;
                break;
            }
        }
    }
    else
    {
        // Normalize common BPIR aliases to actual UE pin names
        FString NormalizedPinName = ExecPinName;
        FString LowerName = ExecPinName.ToLower();
        if (LowerName == TEXT("true"))  { NormalizedPinName = TEXT("then"); }
        else if (LowerName == TEXT("false")) { NormalizedPinName = TEXT("else"); }
        // "Then 4" -> "then_4" (human-friendly to UE internal format)
        NormalizedPinName.ReplaceInline(TEXT(" "), TEXT("_"));

        // Find exec output pin matching the specified name (case-insensitive)
        TArray<FString> AvailableExecPinNames;
        for (UEdGraphPin* Pin : InsertionPointNode->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                AvailableExecPinNames.Add(Pin->PinName.ToString());
                if (Pin->PinName.ToString().Equals(NormalizedPinName, ESearchCase::IgnoreCase))
                {
                    ExecOutPin = Pin;
                }
            }
        }

        // Auto-create pins on Sequence nodes when the requested pin doesn't exist yet
        if (!ExecOutPin)
        {
            UK2Node_ExecutionSequence* SeqNode = Cast<UK2Node_ExecutionSequence>(InsertionPointNode);
            if (SeqNode)
            {
                constexpr int32 MaxAutoCreateAttempts = 16;
                for (int32 Attempt = 0; Attempt < MaxAutoCreateAttempts && !ExecOutPin; ++Attempt)
                {
                    SeqNode->AddInputPin();
                    // Re-scan for the requested pin after adding
                    for (UEdGraphPin* Pin : InsertionPointNode->Pins)
                    {
                        if (Pin->Direction == EGPD_Output
                            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                            && Pin->PinName.ToString().Equals(NormalizedPinName, ESearchCase::IgnoreCase))
                        {
                            ExecOutPin = Pin;
                            break;
                        }
                    }
                }
            }
        }

        if (!ExecOutPin)
        {
            // Re-collect pin names after possible auto-creation so the error is accurate
            AvailableExecPinNames.Empty();
            for (UEdGraphPin* Pin : InsertionPointNode->Pins)
            {
                if (Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    AvailableExecPinNames.Add(Pin->PinName.ToString());
                }
            }
            FString AvailableList = FString::Join(AvailableExecPinNames, TEXT(", "));
            return FCompileResult::MakeError(-1,
                FString::Printf(TEXT("InsertCodeAfterNode: no exec output pin named '%s'. Available: [%s]"),
                    *ExecPinName, *AvailableList));
        }
    }

    if (!ExecOutPin)
    {
        return FCompileResult::MakeError(-1, TEXT("InsertCodeAfterNode: insertion node has no exec output pin."));
    }

    FScopedTransaction Transaction(NSLOCTEXT("BpirCompiler", "InsertCode", "Insert BPIR Code"));
    TargetBlueprint->Modify();

    // Save and disconnect ALL existing downstream connections
    TArray<UEdGraphPin*> DownstreamPins;
    if (ExecOutPin->LinkedTo.Num() > 0)
    {
        DownstreamPins = ExecOutPin->LinkedTo;
        ExecOutPin->BreakAllPinLinks();
    }

    // Reset per-compile state
    bCompileReplaceMode = false;
    NodeEmitter->GetCreatedNodeGUIDs().Empty();
    AccumulatedErrors.Empty();
    AccumulatedWarnings.Empty();
    PinResolver->Clear();

    // Switch graph context to the insertion node's graph
    UEdGraph* InsertionGraph = InsertionPointNode->GetGraph();
    if (InsertionGraph)
    {
        CurrentGraph = InsertionGraph;
        NodeEmitter->SetGraph(InsertionGraph);
        ValueResolver->SetGraph(InsertionGraph);
    }

    // Register anchor node's non-exec, non-delegate output pins in PinResolver
    // so inserted BPIR body can reference custom event parameters, function params, etc.
    for (UEdGraphPin* Pin : InsertionPointNode->Pins)
    {
        if (Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate
            && !Pin->PinName.IsNone()
            && Pin->PinName != UEdGraphSchema_K2::PN_Then)
        {
            PinResolver->RegisterVariable(Pin->PinName.ToString(), Pin);
        }
    }

    // Inject externally-provided pin references (from RPC "context" param)
    for (const auto& Pair : PendingExternalInjections)
    {
        ValueResolver->InjectCachedVariable(Pair.Key, Pair.Value);
    }

    // Pass 1: Parse headless body
    FBpirEntryBlock Block;
    TArray<FCompileError> ParseErrors;
    FBpirParser Parser;

    if (!Parser.ParseBody(Code, Block, ParseErrors))
    {
        // Restore downstream connections on parse failure
        for (UEdGraphPin* DsPin : DownstreamPins)
        {
            const UEdGraphSchema* RestoreSchema = ExecOutPin->GetSchema();
            if (RestoreSchema)
            {
                RestoreSchema->TryCreateConnection(ExecOutPin, DsPin);
            }
        }

        PendingExternalInjections.Empty();
        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = ParseErrors;
        return ErrorResult;
    }

    // Pass 0: Force-load all externally-referenced UClass paths before any graph mutation.
    PreloadExternalClasses(MakeArrayView(&Block, 1));

    // Clear emit state
    EmitMap.Empty();
    ValueResolver->SetEmitMap(&EmitMap);

    const int32 BlockCreatedStartIndex = NodeEmitter->GetCreatedNodeGUIDs().Num();

    // Pass 2b: Pre-emit VariableGet/Self nodes for all $var and self references
    PreEmitVariableRefs(Block);

    // Pass 2c: Emit K2 nodes for each instruction
    UEdGraphPin* CurrentExecPin = ExecOutPin;
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        FBpirInstruction& Inst = Block.Instructions[i];
        if (Inst.Opcode == EBpirOpcode::Label)
        {
            CurrentExecPin = nullptr;
            continue;
        }
        if (Inst.Opcode == EBpirOpcode::Comment)
        {
            continue;
        }
        if (Inst.Opcode == EBpirOpcode::ExecGoto || Inst.Opcode == EBpirOpcode::End)
        {
            CurrentExecPin = nullptr;
            continue;
        }

        if (!EmitInstruction(i, Inst, Block, CurrentExecPin))
        {
            UE_LOG(LogBpirCompiler, Warning, TEXT("EmitInstruction failed for opcode %d at line %d"),
                static_cast<int32>(Inst.Opcode), Inst.SourceLine);
            AccumulatedErrors.Add(FCompileError{Inst.SourceLine,
                FString::Printf(TEXT("EmitInstruction failed for opcode %d"), static_cast<int32>(Inst.Opcode))});
        }

        if (const FEmittedNodeInfo* Emitted = EmitMap.Find(i))
        {
            ApplyBpirEnabledState(Inst.EnabledState, Inst.bHasEnabledState, Emitted->Node);
        }

        // Multi-output nodes manage their own exec wiring via labels
        if (Inst.HasExecTargets())
        {
            CurrentExecPin = nullptr;
        }
    }

    FAuthoredPlacementMode PlacementMode = ApplyAuthoredPositionsForBlock(Block, EmitMap, AccumulatedErrors);

    // Pass 3a: Wire data pins
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        FBpirInstruction& Inst = Block.Instructions[i];
        const FEmittedNodeInfo* Info = EmitMap.Find(i);
        if (Inst.Args.Num() > 0 && Info && Info->Node && !Info->bSkipWireDataPins)
        {
            if (!WireDataPins(i, Inst, Block))
            {
                // Errors already added to AccumulatedErrors by WireDataPins
            }
        }
    }

    // Pass 3b: Wire exec pins
    if (!WireExecPins(Block, ExecOutPin))
    {
        // Errors already added to AccumulatedErrors by WireExecPins
    }

    RejectImplicitVisibleHelpersForAuthoredBlock(
        Block,
        PlacementMode,
        NodeEmitter->GetCreatedNodeGUIDs(),
        BlockCreatedStartIndex,
        TargetBlueprint,
        AccumulatedErrors);

    // Re-attach the previously disconnected downstream nodes to the last exec pin.
    //
    // Selection of the reattachment source pin (per
    // B-bpir-statement-cast-success-unwired-replace-shared-topology #2):
    //
    // Scan backward through the body's instructions for the last impure that can
    // anchor the downstream pins. Three shapes matter:
    //
    //   (a) trailing empty label after a labeled-exec node — e.g. body terminates
    //       at `branch [true -> @continue]` followed by `@continue:` with no
    //       impures. The branch's True pin is the actual exit, and that target
    //       label is empty so Step 3 cannot wire it to anything. Reattach the
    //       downstream pins to that labeled exit pin directly (and skip the
    //       generic ExecOutputPin — it was never going to be reached).
    //
    //   (b) trailing instruction has a `then` / `Completed` (non-labeled) exit pin —
    //       reattach to that pin (the existing ExecOutputPin selection).
    //
    //   (c) trailing instruction is a Cast etc whose ExecOutputPin is null — keep
    //       scanning upward.
    //
    // A trailing `end` is different: it deliberately has no successor, so stop
    // before selecting an earlier node as a downstream reattachment source.
    if (DownstreamPins.Num() > 0)
    {
        UEdGraphPin* LastExecPin = nullptr;

        for (int32 i = Block.Instructions.Num() - 1; i >= 0; --i)
        {
            FBpirInstruction& Inst = Block.Instructions[i];
            if (Inst.Opcode == EBpirOpcode::End)
            {
                break;
            }
            const FEmittedNodeInfo* Info = EmitMap.Find(i);
            if (!Inst.IsImpure() || !Info || !Info->Node)
            {
                continue;
            }

            // Case (a): instruction has labeled exec targets that resolve to
            // empty/missing-impure labels. Reattach to the labeled exit pin.
            if (Inst.HasExecTargets())
            {
                UEdGraphPin* EmptyLabelExitPin = nullptr;
                bool bAnyTargetResolved = false;
                for (const FBpirExecTarget& Target : Inst.ExecTargets)
                {
                    const int32 TargetIdx = FindFirstImpureAtLabel(Target.Label, Block);
                    if (TargetIdx >= 0)
                    {
                        bAnyTargetResolved = true;
                        continue;
                    }
                    // Empty convergence label — candidate reattach point.
                    if (UEdGraphPin* Pin = FindExecOutputPin(i, Inst, Target.PinName))
                    {
                        if (Pin->LinkedTo.Num() == 0)
                        {
                            EmptyLabelExitPin = Pin;
                            // Don't break — prefer the first empty-label exit, but keep scanning
                            // so bAnyTargetResolved sees every target.
                        }
                    }
                }

                if (EmptyLabelExitPin)
                {
                    LastExecPin = EmptyLabelExitPin;
                    UE_LOG(LogBpirCompiler, Verbose,
                        TEXT("InsertCodeAfterNode.Reattach: selected labeled exit pin '%s' on inst[%d]=%s (empty trailing label)"),
                        *EmptyLabelExitPin->PinName.ToString(), i, BpirOpcodeName(Inst.Opcode));
                    break;
                }

                // All targets resolved — the labeled-exec node is fully consumed
                // by its own labels. There is no exit to reattach to; stop the
                // scan (don't fall through to ExecOutputPin which is null on
                // labeled-exec nodes anyway).
                if (bAnyTargetResolved)
                {
                    break;
                }
            }

            // Case (b): non-labeled exit pin (then/Completed).
            if (Info->ExecOutputPin)
            {
                LastExecPin = Info->ExecOutputPin;
                break;
            }

            // Case (c): keep scanning.
        }

        if (LastExecPin && LastExecPin->LinkedTo.Num() == 0)
        {
            for (UEdGraphPin* DsPin : DownstreamPins)
            {
                const UEdGraphSchema* Schema = LastExecPin->GetSchema();
                if (Schema)
                {
                    const UEdGraphNode* SrcNode = LastExecPin->GetOwningNode();
                    const UEdGraphNode* DstNode = DsPin->GetOwningNode();
                    if (!Schema->TryCreateConnection(LastExecPin, DsPin))
                    {
                        UE_LOG(LogBpirCompiler, Warning,
                            TEXT("InsertCodeAfterNode.Reattach: TryCreateConnection FAILED src=%s(guid=%s).%s dst=%s(guid=%s).%s"),
                            SrcNode ? *SrcNode->GetClass()->GetName() : TEXT("<null>"),
                            SrcNode ? *SrcNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                            *LastExecPin->PinName.ToString(),
                            DstNode ? *DstNode->GetClass()->GetName() : TEXT("<null>"),
                            DstNode ? *DstNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                            *DsPin->PinName.ToString());
                        AccumulatedErrors.Add(FCompileError(-1,
                            FString::Printf(TEXT("TryCreateConnection failed re-wiring downstream exec '%s' -> '%s'"),
                                *LastExecPin->PinName.ToString(), *DsPin->PinName.ToString())));
                    }
                    else
                    {
                        UE_LOG(LogBpirCompiler, Verbose,
                            TEXT("InsertCodeAfterNode.Reattach: TryCreateConnection OK src=%s(guid=%s).%s dst=%s(guid=%s).%s"),
                            SrcNode ? *SrcNode->GetClass()->GetName() : TEXT("<null>"),
                            SrcNode ? *SrcNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                            *LastExecPin->PinName.ToString(),
                            DstNode ? *DstNode->GetClass()->GetName() : TEXT("<null>"),
                            DstNode ? *DstNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                            *DsPin->PinName.ToString());
                    }
                }
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(TargetBlueprint);

    TArray<FGuid> CreatedGUIDs = NodeEmitter->GetCreatedNodeGUIDs();

    // Atomic rollback on error
    if (AccumulatedErrors.Num() > 0)
    {
        DeleteNodesByGUIDs(TargetBlueprint, CreatedGUIDs);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(TargetBlueprint);

        // Restore downstream connections
        for (UEdGraphPin* DsPin : DownstreamPins)
        {
            const UEdGraphSchema* RestoreSchema = ExecOutPin->GetSchema();
            if (RestoreSchema)
            {
                RestoreSchema->TryCreateConnection(ExecOutPin, DsPin);
            }
        }

        PendingExternalInjections.Empty();
        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = AccumulatedErrors;
        return ErrorResult;
    }

    PendingExternalInjections.Empty();

    // --- Layout pass ---
    {
        UEdGraph* LayoutGraph = InsertionPointNode ? InsertionPointNode->GetGraph() : nullptr;
        RunLayoutPass(TargetBlueprint, ExcludeAuthoredPlacementGuids(CreatedGUIDs, PlacementMode.PrimaryNodeGuids), LayoutGraph, {}, InsertionPointNode);
    }

    FPluginState::Get().NodeCreationStack().Add(CreatedGUIDs);
    // InsertCodeAfterNode has no Phase 0 sweeps — push false so undo_last_bpir can roll back normally.
    FPluginState::Get().Phase0RanStack().Add(false);
    FCompileResult SuccessResult = FCompileResult::MakeSuccess(CreatedGUIDs);
    SuccessResult.Warnings = AccumulatedWarnings;
    return SuccessResult;
}

// ----------------------------------------------------------------------------
// InsertCodeBeforeNode()
// ----------------------------------------------------------------------------

FCompileResult FBpirCompiler::InsertCodeBeforeNode(UEdGraphNode* TargetNode, const FString& Code)
{
    if (!TargetNode)
    {
        return FCompileResult::MakeError(-1, TEXT("InsertCodeBeforeNode: null target node."));
    }

    // Find the target's exec input pin that has an upstream connection
    UEdGraphPin* ExecInPin = nullptr;
    for (UEdGraphPin* Pin : TargetNode->Pins)
    {
        if (Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            && Pin->LinkedTo.Num() > 0)
        {
            ExecInPin = Pin;
            break;
        }
    }

    if (!ExecInPin)
    {
        return FCompileResult::MakeError(-1, TEXT("InsertCodeBeforeNode: no upstream connection found — cannot insert before an entry/event node."));
    }

    // Get the upstream pin and its owning node
    UEdGraphPin* UpstreamPin = ExecInPin->LinkedTo[0];
    UEdGraphNode* UpstreamNode = UpstreamPin->GetOwningNode();
    FString UpstreamPinName = UpstreamPin->PinName.ToString();

    // Delegate to InsertCodeAfterNode with the upstream node and pin name
    return InsertCodeAfterNode(UpstreamNode, Code, UpstreamPinName);
}

// ----------------------------------------------------------------------------
// InjectExternalVariable()
// ----------------------------------------------------------------------------

void FBpirCompiler::InjectExternalVariable(const FString& VarName, UEdGraphPin* Pin)
{
    if (Pin)
    {
        PendingExternalInjections.Add(VarName, Pin);
    }
}

void FBpirCompiler::SetExitTunnel(UK2Node_Tunnel* ExitTunnel)
{
    CurrentMacroExitTunnel = ExitTunnel;
}

// ----------------------------------------------------------------------------
// CompileBodyIntoGraph()
// ----------------------------------------------------------------------------

FCompileResult FBpirCompiler::CompileBodyIntoGraph(const FString& BodyCode, UEdGraph* TargetGraph, UEdGraphPin* EntryExecPin)
{
    if (!TargetGraph)
    {
        return FCompileResult::MakeError(-1, TEXT("CompileBodyIntoGraph: null TargetGraph."));
    }

    // Reset per-compile state
    bCompileReplaceMode = false;
    NodeEmitter->GetCreatedNodeGUIDs().Empty();
    AccumulatedErrors.Empty();
    AccumulatedWarnings.Empty();
    PinResolver->Clear();

    // Switch graph context
    CurrentGraph = TargetGraph;
    NodeEmitter->SetGraph(TargetGraph);
    ValueResolver->SetGraph(TargetGraph);

    for (const auto& Pair : PendingExternalInjections)
    {
        ValueResolver->InjectCachedVariable(Pair.Key, Pair.Value);
    }

    // Parse body
    FBpirEntryBlock Block;
    TArray<FCompileError> ParseErrors;
    FBpirParser Parser;

    if (!Parser.ParseBody(BodyCode, Block, ParseErrors))
    {
        PendingExternalInjections.Empty();

        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = ParseErrors;
        return ErrorResult;
    }

    // Pass 0: Force-load all externally-referenced UClass paths before any graph mutation.
    PreloadExternalClasses(MakeArrayView(&Block, 1));

    // Clear emit state
    EmitMap.Empty();
    ValueResolver->SetEmitMap(&EmitMap);

    const int32 BlockCreatedStartIndex = NodeEmitter->GetCreatedNodeGUIDs().Num();

    // Pre-emit $var/self/external references
    PreEmitVariableRefs(Block);

    // Layout placement
    if (EntryExecPin)
    {
        NodeEmitter->ResetPlacementForChain(EntryExecPin, 0);
    }

    // Emit K2 nodes
    UEdGraphPin* CurrentExecPin = EntryExecPin;
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        FBpirInstruction& Inst = Block.Instructions[i];
        if (Inst.Opcode == EBpirOpcode::Label)
        {
            CurrentExecPin = nullptr;
            continue;
        }
        if (Inst.Opcode == EBpirOpcode::Comment)
        {
            continue;
        }
        if (Inst.Opcode == EBpirOpcode::ExecGoto || Inst.Opcode == EBpirOpcode::End)
        {
            CurrentExecPin = nullptr;
            continue;
        }

        if (!EmitInstruction(i, Inst, Block, CurrentExecPin))
        {
            UE_LOG(LogBpirCompiler, Warning, TEXT("EmitInstruction failed for opcode %d at line %d"),
                static_cast<int32>(Inst.Opcode), Inst.SourceLine);
            AccumulatedErrors.Add(FCompileError{Inst.SourceLine,
                FString::Printf(TEXT("EmitInstruction failed for opcode %d"), static_cast<int32>(Inst.Opcode))});
        }

        if (const FEmittedNodeInfo* Emitted = EmitMap.Find(i))
        {
            ApplyBpirEnabledState(Inst.EnabledState, Inst.bHasEnabledState, Emitted->Node);
        }

        if (Inst.HasExecTargets())
        {
            CurrentExecPin = nullptr;
        }
    }

    FAuthoredPlacementMode PlacementMode = ApplyAuthoredPositionsForBlock(Block, EmitMap, AccumulatedErrors);

    // Wire data pins
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        FBpirInstruction& Inst = Block.Instructions[i];
        const FEmittedNodeInfo* Info = EmitMap.Find(i);
        if (Inst.Args.Num() > 0 && Info && Info->Node && !Info->bSkipWireDataPins)
        {
            if (!WireDataPins(i, Inst, Block))
            {
                // Errors already added to AccumulatedErrors by WireDataPins
            }
        }
    }

    // Wire exec pins
    if (!WireExecPins(Block, EntryExecPin))
    {
        // Errors already added to AccumulatedErrors by WireExecPins
    }

    RejectImplicitVisibleHelpersForAuthoredBlock(
        Block,
        PlacementMode,
        NodeEmitter->GetCreatedNodeGUIDs(),
        BlockCreatedStartIndex,
        TargetBlueprint,
        AccumulatedErrors);

    TArray<FGuid> CreatedGUIDs = NodeEmitter->GetCreatedNodeGUIDs();

    // Rollback on error
    if (AccumulatedErrors.Num() > 0)
    {
        DeleteNodesByGUIDs(TargetBlueprint, CreatedGUIDs);
        PendingExternalInjections.Empty();

        FCompileResult ErrorResult;
        ErrorResult.bSuccess = false;
        ErrorResult.Errors = AccumulatedErrors;
        return ErrorResult;
    }

    // Find the last exec output pin for the caller
    UEdGraphPin* LastExecPin = CurrentExecPin;
    if (!LastExecPin)
    {
        for (int32 i = Block.Instructions.Num() - 1; i >= 0; --i)
        {
            FBpirInstruction& Inst = Block.Instructions[i];
            if (Inst.Opcode == EBpirOpcode::End)
            {
                break;
            }
            const FEmittedNodeInfo* Info = EmitMap.Find(i);
            if (Inst.IsImpure() && Info && Info->Node && Info->ExecOutputPin)
            {
                LastExecPin = Info->ExecOutputPin;
                break;
            }
        }
    }

    PendingExternalInjections.Empty();

    // --- Layout pass ---
    {
        UEdGraphNode* AnchorNode = EntryExecPin ? EntryExecPin->GetOwningNode() : nullptr;
        RunLayoutPass(TargetBlueprint, ExcludeAuthoredPlacementGuids(CreatedGUIDs, PlacementMode.PrimaryNodeGuids), TargetGraph, {}, AnchorNode);
    }

    FCompileResult Result = FCompileResult::MakeSuccess(CreatedGUIDs);
    Result.LastExecOutputPin = LastExecPin;
    Result.Warnings = AccumulatedWarnings;
    return Result;
}

// ----------------------------------------------------------------------------
// SwitchToGraph() — update all graph-dependent state in one call
// ----------------------------------------------------------------------------

void FBpirCompiler::SwitchToGraph(UEdGraph* NewGraph)
{
    CurrentGraph = NewGraph;
    NodeEmitter->SetGraph(NewGraph);
    ValueResolver->SetGraph(NewGraph);
    CurrentMacroExitTunnel = nullptr;
}

// ----------------------------------------------------------------------------
// PreEmitVariableRefs() — scan block instructions for $var, self, and Set
// TypeArg references, pre-emitting the VariableGet/Self/ExternalGet nodes
// that WireDataPins will need later. Shared by Compile(), InsertCodeAfterNode(),
// and CompileBodyIntoGraph().
// ----------------------------------------------------------------------------

void FBpirCompiler::PreEmitVariableRefs(FBpirEntryBlock& Block)
{
    // First pass: collect the set of alias ResultNames that are actually referenced
    // by a later instruction (via %ref args, or transitively via another alias's RHS).
    // Dead aliases — assignments whose %name is never read — don't need their RHS
    // pre-emitted, because nothing downstream will look up the VariableGet.
    // Eagerly emitting for them creates spurious unused graph nodes.
    TSet<FString> ReferencedAliasNames;
    {
        auto AddIfPercentRef = [&ReferencedAliasNames, &Block](const FString& Value)
        {
            if (!Value.StartsWith(TEXT("%"))) return;
            FString Name = Value.Mid(1);
            int32 Dot = INDEX_NONE;
            if (Name.FindChar(TEXT('.'), Dot)) Name = Name.Left(Dot);
            if (Block.ValueIndex.Contains(Name))
            {
                ReferencedAliasNames.Add(Name);
            }
        };

        for (const FBpirInstruction& Inst : Block.Instructions)
        {
            for (const FBpirArg& Arg : Inst.Args)
            {
                AddIfPercentRef(Arg.Value);
            }
            AddIfPercentRef(Inst.TypeArg);
            if (Inst.Opcode == EBpirOpcode::Alias)
            {
                AddIfPercentRef(Inst.AliasRhs);
            }
        }
    }

    // Per-value pre-emit. Encapsulated so the cast<T>(inner) branch can recurse
    // on its inner expression with the same $-ref / self handling.
    auto PreEmitValueRef = [this](const FString& Value, auto& PreEmitValueRefRef, int32 Depth) -> void
    {
        // Bounded recursion: cap nested cast<...>(...) wrappers to avoid pathological input.
        constexpr int32 MaxCastDepth = 4;
        if (Depth > MaxCastDepth)
        {
            return;
        }

        if (Value.StartsWith(TEXT("$")))
        {
            const FString VarRef = Value.Mid(1);
            FString Target;
            FString Property;
            if (SplitDollarReference(VarRef, Target, Property))
            {
                ValueResolver->PreEmitDollarVar(Target);
                ValueResolver->PreEmitExternalGet(Target, Property);
            }
            else
            {
                ValueResolver->PreEmitDollarVar(Target);
            }
        }
        else if (Value == TEXT("self"))
        {
            ValueResolver->PreEmitSelf();
        }
        else if (Value.StartsWith(TEXT("cast<")))
        {
            FString CastType;
            FString Inner;
            if (FBpirValueResolver::ParseCastSyntax(Value, CastType, Inner) && !Inner.IsEmpty())
            {
                PreEmitValueRefRef(Inner, PreEmitValueRefRef, Depth + 1);
            }
        }
    };

    for (const FBpirInstruction& Inst : Block.Instructions)
    {
        for (const FBpirArg& Arg : Inst.Args)
        {
            PreEmitValueRef(Arg.Value, PreEmitValueRef, 0);
        }
        if (Inst.Opcode == EBpirOpcode::Set && Inst.TypeArg.StartsWith(TEXT("$")))
        {
            FString TargetVarName;
            FString PropertyName;
            SplitDollarReference(Inst.TypeArg.Mid(1), TargetVarName, PropertyName);
            ValueResolver->PreEmitDollarVar(TargetVarName);
        }
        // Alias instructions: pre-emit the $var RHS only if this alias's ResultName
        // is actually referenced downstream. Skipping dead aliases avoids emitting
        // orphan VariableGet nodes that pad the graph without contributing wiring.
        if (Inst.Opcode == EBpirOpcode::Alias
            && Inst.AliasRhs.StartsWith(TEXT("$"))
            && ReferencedAliasNames.Contains(Inst.ResultName))
        {
            ValueResolver->PreEmitDollarVar(NormalizeBpirNameToken(Inst.AliasRhs.Mid(1)));
        }
    }
}

// ----------------------------------------------------------------------------
// PreloadExternalClasses() — Pass 0: force-load all UClass paths referenced by
// TypeArg and Arg.Value across every instruction so that subsequent ResolveUClass
// calls inside Emit/Wire hit FindObject fast paths and never trigger
// LoadObject -> FlushAsyncLoading -> FlushCompilationQueue re-entrance.
// This avoids class-load re-entrance while the compiler is mutating graphs.
// ----------------------------------------------------------------------------

void FBpirCompiler::PreloadExternalClasses(TArrayView<const FBpirEntryBlock> Blocks)
{
    // Collect unique class-candidate strings from TypeArg and Arg.Value across all instructions,
    // then force-load them via ResolveUClass. After this pass, subsequent ResolveUClass calls
    // during Emit/Wire hit in-memory fast paths and never trigger LoadObject/FlushAsyncLoading.
    TSet<FString> Candidates;

    auto AddCandidate = [&Candidates](const FString& Value)
    {
        if (Value.IsEmpty())                   return;
        if (Value.Equals(TEXT("self")))        return;
        if (Value.StartsWith(TEXT("%")))       return;   // local ref
        if (Value.StartsWith(TEXT("$")))       return;   // external var ref
        if (Value.Contains(TEXT("::")))        return;   // enum literal
        if (Value.StartsWith(TEXT("\"")))      return;   // quoted string literal
        if (Value.StartsWith(TEXT("(")))       return;   // struct/gameplay-tag literal
        // Reject composite expressions (e.g. `cast<Actor>($Source)`) — they aren't
        // class paths, and asking LoadObject to resolve them spews bogus
        // "Failed to find object 'Class None.<expr>'" warnings into the log.
        if (Value.Contains(TEXT("<")) ||
            Value.Contains(TEXT("(")))         return;
        // Pure numeric literal (int/float, optional sign, optional decimal)
        bool bAllNumeric = true;
        for (int32 i = 0; i < Value.Len(); ++i)
        {
            const TCHAR C = Value[i];
            if (C == TEXT('-') || C == TEXT('+') || C == TEXT('.') ||
                (C >= TEXT('0') && C <= TEXT('9')))
            {
                continue;
            }
            bAllNumeric = false;
            break;
        }
        if (bAllNumeric && Value.Len() > 0) return;

        Candidates.Add(Value);
    };

    for (const FBpirEntryBlock& Block : Blocks)
    {
        for (const FBpirInstruction& Inst : Block.Instructions)
        {
            if (!Inst.TypeArg.IsEmpty())
            {
                AddCandidate(Inst.TypeArg);
            }
            for (const FBpirArg& Arg : Inst.Args)
            {
                AddCandidate(Arg.Value);
            }
        }
    }

    for (const FString& Candidate : Candidates)
    {
        // Discard result — purpose is to force the package into memory so later
        // ResolveUClass calls inside Emit/Wire resolve via FindObject without LoadObject.
        (void)ResolveUClass(Candidate);
    }
}

// ----------------------------------------------------------------------------
// SetupEntryPoint() — dispatches by Block.Kind
// ----------------------------------------------------------------------------

UEdGraphPin* FBpirCompiler::SetupEntryPoint(FBpirEntryBlock& Block)
{
    switch (Block.Kind)
    {
    case EBpirEntryKind::Event:
    {
        // Class-aware: keep "Tick" on UUserWidget; only rewrite to "ReceiveTick" when
        // the parent is an AActor subclass and the literal name doesn't already exist.
        const FName Resolved = ResolveOverrideEventName(Block.Name, TargetBlueprint ? TargetBlueprint->ParentClass : nullptr);
        return SetupBuiltinEvent(Resolved.ToString());
    }

    case EBpirEntryKind::CustomEvent:
        return SetupCustomEvent(Block.Name, Block.Params, bCompileReplaceMode, Block.Metadata);

    case EBpirEntryKind::Function:
    {
        BlueprintHandlerUtils::FBlueprintOverrideInfo OverrideInfo;
        FString OverrideError;
        bool bIsInterfaceOwned = false;
        const bool bHasLocalFunctionGraph = BlueprintHandlerUtils::FindFunctionGraphByName(
            TargetBlueprint,
            Block.Name,
            &bIsInterfaceOwned) != nullptr;
        const bool bCanAutoOverride = BlueprintHandlerUtils::TryResolveBlueprintOverride(
            TargetBlueprint,
            Block.Name,
            OverrideInfo,
            OverrideError);
        if (bCanAutoOverride && (!bHasLocalFunctionGraph || bIsInterfaceOwned))
        {
            if (!HasExplicitOverrideSignature(Block))
            {
                return SetupOverride(Block.Name, Block.Params, Block.ReturnType, Block.OutputParams, CompileMode, Block.Metadata);
            }

            TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ExpectedInputs;
            TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ExpectedOutputs;
            FString SignatureBuildError;
            if (!BuildExpectedOverrideSignature(Block, ExpectedInputs, ExpectedOutputs, SignatureBuildError))
            {
                AccumulatedErrors.Add(FCompileError(-1, SignatureBuildError));
                return nullptr;
            }

            TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ActualInputs;
            TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ActualOutputs;
            FString SignatureError;
            if (!BlueprintHandlerUtils::GetFunctionSignatureDescriptors(
                OverrideInfo.Function,
                ActualInputs,
                ActualOutputs,
                SignatureError))
            {
                AccumulatedErrors.Add(FCompileError(-1, SignatureError));
                return nullptr;
            }

            FString MismatchMessage;
            if (BlueprintHandlerUtils::DoPinTypeDescriptorsMatch(
                ExpectedInputs,
                ExpectedOutputs,
                ActualInputs,
                ActualOutputs,
                MismatchMessage))
            {
                return SetupOverride(Block.Name, Block.Params, Block.ReturnType, Block.OutputParams, CompileMode, Block.Metadata);
            }

            AccumulatedErrors.Add(FCompileError(
                -1,
                FString::Printf(
                    TEXT("Function '%s' matches an overridable parent function, but the authored signature does not match the parent override signature: %s"),
                    *Block.Name,
                    *MismatchMessage)));
            return nullptr;
        }

        return SetupFunction(Block.Name, Block.ReturnType, Block.Params, Block.OutputParams, Block.Metadata);
    }

    case EBpirEntryKind::Override:
        return SetupOverride(Block.Name, Block.Params, Block.ReturnType, Block.OutputParams, CompileMode, Block.Metadata);

    case EBpirEntryKind::Construction:
        return SetupConstructionScript();

    case EBpirEntryKind::ComponentEvent:
        return SetupComponentEvent(Block.ComponentName, Block.Name);

    case EBpirEntryKind::WidgetEvent:
        return SetupWidgetEvent(Block.ComponentName, Block.Name, Block.Params);

    case EBpirEntryKind::KeyPressed:
        return SetupKeyEvent(Block.Name, false);

    case EBpirEntryKind::KeyReleased:
        return SetupKeyEvent(Block.Name, true);

    case EBpirEntryKind::InputAction:
        return SetupInputActionEvent(Block.Name);

    case EBpirEntryKind::Macro:
        return SetupMacro(Block.Name, Block.Params, Block.OutputParams, Block.ExecOutputNames, Block);

    default:
        UE_LOG(LogBpirCompiler, Warning, TEXT("Unknown entry kind: %d"), static_cast<int32>(Block.Kind));
        return nullptr;
    }
}

// ----------------------------------------------------------------------------
// Entry Setup Methods
// ----------------------------------------------------------------------------

UEdGraphPin* FBpirCompiler::SetupBuiltinEvent(const FString& EventName)
{
    if (!TargetBlueprint || !CurrentGraph)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupBuiltinEvent: no blueprint or graph"));
        return nullptr;
    }

    UK2Node_Event* EventNode = NodeEmitter->CreateEventNode(FName(*EventName));
    if (!EventNode)
    {
        UE_LOG(LogBpirCompiler, Warning,
            TEXT("SetupBuiltinEvent: CreateEventNode failed for '%s'"), *EventName);
        return nullptr;
    }

    // Reset layout origin to this node's position
    NodeEmitter->ResetPlacementForChain(
        EventNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);

    // Register every output data pin on the event node so BPIR body code can
    // reference its parameters via PinResolver. AllocateDefaultPins (called by
    // CreateEventNode) already populated the pins from the parent UFUNCTION,
    // so this works for arbitrary BlueprintImplementableEvent / BlueprintNativeEvent
    // overrides — not just the ~5 hardcoded engine events that were special-cased before.
    // Mirrors the loop in SetupOverride; keep the two in sync.
    for (UEdGraphPin* Pin : EventNode->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Output)
        {
            continue;
        }
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
        {
            continue;
        }
        if (Pin->PinName == UEdGraphSchema_K2::PN_Then
            || Pin->PinName == UEdGraphSchema_K2::PN_Self)
        {
            continue;
        }
        PinResolver->RegisterVariable(Pin->PinName.ToString(), Pin);
    }

    return EventNode->FindPin(UEdGraphSchema_K2::PN_Then);
}

UEdGraphPin* FBpirCompiler::SetupCustomEvent(
    const FString& Name, const TArray<FBpirEntryBlock::FParam>& Params, bool bReplaceMode, const FBpirEntryMetadata& Metadata)
{
    if (!CurrentGraph)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupCustomEvent: no current graph"));
        return nullptr;
    }

    // --- Duplicate detection: search existing graphs for a K2Node_CustomEvent with the same name ---
    UK2Node_CustomEvent* ExistingNode = nullptr;
    for (UEdGraph* Graph : TargetBlueprint->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
            if (CE && CE->CustomFunctionName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                ExistingNode = CE;
                break;
            }
        }
        if (ExistingNode) { break; }
    }

    if (ExistingNode)
    {
        // Collect parameter pins from the existing node (output, non-exec, non-delegate, not "then")
        TArray<UEdGraphPin*> ExistingParamPins;
        for (UEdGraphPin* Pin : ExistingNode->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate
                && Pin->PinName != UEdGraphSchema_K2::PN_Then)
            {
                ExistingParamPins.Add(Pin);
            }
        }

        // Build expected parameter pin types from the requested params
        TArray<TPair<FString, FEdGraphPinType>> RequestedParams;
        for (const FBpirEntryBlock::FParam& Param : Params)
        {
            if (Param.Name.IsEmpty()) { continue; }
            FEdGraphPinType PinType;
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
            FCodePinResolver::ConvertTypeSpecToPinType(Param.Type, PinType);
            RequestedParams.Emplace(Param.Name, PinType);
        }

        // Compare signatures: count, names (case-insensitive), and types (category + subcategory object)
        bool bSignatureMatch = (ExistingParamPins.Num() == RequestedParams.Num());
        if (bSignatureMatch)
        {
            for (int32 i = 0; i < RequestedParams.Num(); ++i)
            {
                const UEdGraphPin* ExPin = ExistingParamPins[i];
                const TPair<FString, FEdGraphPinType>& Req = RequestedParams[i];
                if (!ExPin->PinName.ToString().Equals(Req.Key, ESearchCase::IgnoreCase)
                    || ExPin->PinType.PinCategory != Req.Value.PinCategory
                    || ExPin->PinType.PinSubCategoryObject != Req.Value.PinSubCategoryObject
                    || ExPin->PinType.ContainerType != Req.Value.ContainerType)
                {
                    bSignatureMatch = false;
                    break;
                }
            }
        }

        if (bSignatureMatch)
        {
            // Same signature — reuse existing node (idempotent re-compilation)
            UE_LOG(LogBpirCompiler, Log, TEXT("SetupCustomEvent: reusing existing custom event '%s' with matching signature"), *Name);
            AccumulatedWarnings.Add(FString::Printf(TEXT("Custom event '%s' already exists with matching signature — reusing existing node (idempotent re-compilation)."), *Name));
            CreatedCustomEvents.Add(Name, ExistingNode);

            ApplyEntryMetadataToCustomEvent(ExistingNode, Metadata);

            // Register parameter output pins so body code can reference them
            for (const FBpirEntryBlock::FParam& Param : Params)
            {
                if (Param.Name.IsEmpty()) { continue; }
                UEdGraphPin* ParamPin = ExistingNode->FindPin(*Param.Name);
                if (ParamPin)
                {
                    PinResolver->RegisterVariable(Param.Name, ParamPin);
                }
            }

            NodeEmitter->ResetPlacementForChain(
                ExistingNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);

            return ExistingNode->FindPin(UEdGraphSchema_K2::PN_Then);
        }
        else if (bReplaceMode)
        {
            // Replace mode: Phase 0 should have deleted the old node, but if RemoveNode
            // failed silently, we still see it here. Remove it now and fall through to
            // create a fresh node with the new signature.
            UE_LOG(LogBpirCompiler, Log,
                TEXT("SetupCustomEvent: replace mode — removing stale custom event '%s' with mismatched signature"),
                *Name);
            FBlueprintEditorUtils::RemoveNode(TargetBlueprint, ExistingNode, /*bDontRecompile=*/true);
            // Fall through to the "No existing node found: create new" path below.
        }
        else
        {
            // Different signature — error, cannot silently replace
            FString NodeGuid = ExistingNode->NodeGuid.ToString();
            FString ErrorMsg = FString::Printf(
                TEXT("Custom event '%s' already exists with a different signature (node GUID: %s). ")
                TEXT("Delete it first via blueprint.remove_event or change the event name."),
                *Name, *NodeGuid);
            UE_LOG(LogBpirCompiler, Warning, TEXT("SetupCustomEvent: %s"), *ErrorMsg);
            AccumulatedErrors.Add(FCompileError{-1, ErrorMsg});
            return nullptr;
        }
    }

    // --- No existing node found: create new ---
    UK2Node_CustomEvent* EventNode = NodeEmitter->CreateCustomEventNode(FName(*Name));
    if (!EventNode)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupCustomEvent: CreateCustomEventNode failed for '%s'"), *Name);
        return nullptr;
    }

    CreatedCustomEvents.Add(Name, EventNode);

    // Add user-defined pins for each parameter
    for (const FBpirEntryBlock::FParam& Param : Params)
    {
        if (Param.Name.IsEmpty()) { continue; }

        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        FCodePinResolver::ConvertTypeSpecToPinType(Param.Type, PinType);

        UEdGraphPin* CreatedPin = EventNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
        if (!CreatedPin)
        {
            UE_LOG(LogBpirCompiler, Warning, TEXT("SetupCustomEvent: CreateUserDefinedPin failed for parameter '%s'"), *Param.Name);
            AccumulatedErrors.Add(FCompileError{-1,
                FString::Printf(TEXT("Failed to create parameter pin '%s' on custom event '%s'"), *Param.Name, *Name)});
        }
    }

    ApplyEntryMetadataToCustomEvent(EventNode, Metadata);

    EventNode->ReconstructNode();

    // Register parameter output pins in PinResolver so body code can reference them
    for (const FBpirEntryBlock::FParam& Param : Params)
    {
        if (Param.Name.IsEmpty()) { continue; }
        UEdGraphPin* ParamPin = EventNode->FindPin(*Param.Name);
        if (ParamPin)
        {
            PinResolver->RegisterVariable(Param.Name, ParamPin);
        }
    }

    NodeEmitter->ResetPlacementForChain(
        EventNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);

    return EventNode->FindPin(UEdGraphSchema_K2::PN_Then);
}

UEdGraphPin* FBpirCompiler::SetupComponentEvent(
    const FString& CompName, const FString& EventName)
{
    if (!CurrentGraph || !TargetBlueprint)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupComponentEvent: no blueprint or graph"));
        return nullptr;
    }

    UK2Node_ComponentBoundEvent* EventNode = NodeEmitter->CreateComponentEventNode(
        FName(*CompName), FName(*EventName), TargetBlueprint);

    if (!EventNode)
    {
        UE_LOG(LogBpirCompiler, Warning,
            TEXT("SetupComponentEvent: CreateComponentEventNode failed for %s::%s"),
            *CompName, *EventName);
        return nullptr;
    }

    // Register every output data pin so arbitrary delegate signatures resolve,
    // not just the historical overlap/hit shape.
    for (UEdGraphPin* Pin : EventNode->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Output)
            continue;
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
            continue;
        if (Pin->PinName == UEdGraphSchema_K2::PN_Then
            || Pin->PinName == UEdGraphSchema_K2::PN_Self)
            continue;
        PinResolver->RegisterVariable(Pin->PinName.ToString(), Pin);
    }

    NodeEmitter->ResetPlacementForChain(
        EventNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);

    return EventNode->FindPin(UEdGraphSchema_K2::PN_Then);
}

UEdGraphPin* FBpirCompiler::SetupWidgetEvent(
    const FString& WidgetName, const FString& EventName,
    const TArray<FBpirEntryBlock::FParam>& Params)
{
    if (!CurrentGraph || !TargetBlueprint)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupWidgetEvent: no blueprint or graph"));
        return nullptr;
    }

    // Widget events use the same ComponentBoundEvent mechanism
    UK2Node_ComponentBoundEvent* EventNode = NodeEmitter->CreateComponentEventNode(
        FName(*WidgetName), FName(*EventName), TargetBlueprint);

    if (!EventNode)
    {
        UE_LOG(LogBpirCompiler, Warning,
            TEXT("SetupWidgetEvent: CreateComponentEventNode failed for %s::%s"),
            *WidgetName, *EventName);
        return nullptr;
    }

    RegisterEntryParamAliases(EventNode, Params);

    NodeEmitter->ResetPlacementForChain(
        EventNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);

    return EventNode->FindPin(UEdGraphSchema_K2::PN_Then);
}

void FBpirCompiler::RegisterEntryParamAliases(
    UEdGraphNode* EntryNode, const TArray<FBpirEntryBlock::FParam>& Params)
{
    if (!EntryNode) return;

    TArray<UEdGraphPin*> DataPins;
    for (UEdGraphPin* Pin : EntryNode->Pins)
    {
        if (!Pin) continue;
        if (Pin->Direction != EGPD_Output) continue;
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate) continue;
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate) continue;
        if (Pin->PinName == UEdGraphSchema_K2::PN_Then) continue;
        if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
        DataPins.Add(Pin);
    }

    // Register canonical pin names first so `$bIsChecked` etc. always resolve,
    // regardless of whether the author declared aliases.
    for (UEdGraphPin* DataPin : DataPins)
    {
        PinResolver->RegisterVariable(DataPin->PinName.ToString(), DataPin);
    }

    // Positionally register author-declared aliases that differ from the canonical name.
    for (int32 i = 0; i < Params.Num(); ++i)
    {
        const FBpirEntryBlock::FParam& Param = Params[i];
        if (Param.Name.IsEmpty()) continue;

        if (!DataPins.IsValidIndex(i))
        {
            UE_LOG(LogBpirCompiler, Warning,
                TEXT("RegisterEntryParamAliases: declared param '%s' at index %d has no "
                     "corresponding data pin on entry node '%s' (node exposes %d data pin(s))"),
                *Param.Name, i, *EntryNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
                DataPins.Num());
            continue;
        }

        UEdGraphPin* DataPin = DataPins[i];
        if (DataPin->PinName.ToString() == Param.Name) continue;

        PinResolver->RegisterVariable(Param.Name, DataPin);
    }
}

UEdGraphPin* FBpirCompiler::SetupKeyEvent(const FString& KeyName, bool bReleased)
{
    if (!CurrentGraph)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupKeyEvent: no current graph"));
        return nullptr;
    }

    UK2Node_InputKey* InputNode = NodeEmitter->CreateInputKeyNode(FName(*KeyName), bReleased);
    if (!InputNode)
    {
        UE_LOG(LogBpirCompiler, Warning,
            TEXT("SetupKeyEvent: CreateInputKeyNode failed for key '%s'"), *KeyName);
        return nullptr;
    }

    // Return the Pressed or Released exec output pin
    UEdGraphPin* ExecOutPin = FBpirInputKeyHelpers::FindInputKeyExecPin(InputNode, bReleased);
    if (!ExecOutPin)
    {
        // Fallback: first output exec pin
        for (UEdGraphPin* Pin : InputNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                ExecOutPin = Pin;
                break;
            }
        }
    }

    NodeEmitter->ResetPlacementForChain(ExecOutPin, 0);
    return ExecOutPin;
}

UEdGraphPin* FBpirCompiler::SetupInputActionEvent(const FString& InputActionPath)
{
    FString LoadError;
    UObject* LoadedObject = ResolveUObjectByPath(InputActionPath, LoadError);
    UInputAction* InputAction = Cast<UInputAction>(LoadedObject);
    if (!InputAction)
    {
        AccumulatedErrors.Add(FCompileError(-1, LoadedObject
            ? FString::Printf(TEXT("Input action asset '%s' has type '%s', expected UInputAction"),
                *InputActionPath, *LoadedObject->GetClass()->GetName())
            : (LoadError.IsEmpty()
                ? FString::Printf(TEXT("Could not load input action asset '%s'"), *InputActionPath)
                : LoadError)));
        return nullptr;
    }

    UClass* NodeClass = FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass();
    if (!NodeClass || !FCodeNodeEmitter::ResolveEnhancedInputActionProperty(NodeClass))
    {
        AccumulatedErrors.Add(FCompileError(-1,
            TEXT("Enhanced Input action node class or InputAction property is unavailable")));
        return nullptr;
    }

    UEdGraphNode* InputNode = NodeEmitter->CreateEnhancedInputActionNode(InputAction);
    UEdGraphPin* TriggeredPin = InputNode
        ? InputNode->FindPin(TEXT("Triggered"), EGPD_Output)
        : nullptr;
    if (!TriggeredPin || TriggeredPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
    {
        AccumulatedErrors.Add(FCompileError(-1, FString::Printf(
            TEXT("Failed to create bound Enhanced Input action node for '%s'"), *InputActionPath)));
        return nullptr;
    }

    NodeEmitter->ResetPlacementForChain(TriggeredPin, 0);
    return TriggeredPin;
}

UEdGraphPin* FBpirCompiler::SetupConstructionScript()
{
    if (!TargetBlueprint)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupConstructionScript: no blueprint"));
        return nullptr;
    }

    UEdGraph* ConstructionGraph = FBlueprintEditorUtils::FindUserConstructionScript(TargetBlueprint);
    if (!ConstructionGraph)
    {
        UE_LOG(LogBpirCompiler, Warning,
            TEXT("SetupConstructionScript: FindUserConstructionScript returned null"));
        return nullptr;
    }

    // Switch compiler context to the construction script graph
    CurrentGraph = ConstructionGraph;
    NodeEmitter->SetGraph(ConstructionGraph);
    ValueResolver->SetGraph(ConstructionGraph);

    UK2Node_FunctionEntry* EntryNode = NodeEmitter->GetOrCreateFunctionEntry(ConstructionGraph);
    if (!EntryNode)
    {
        UE_LOG(LogBpirCompiler, Warning,
            TEXT("SetupConstructionScript: could not get/create FunctionEntry node"));
        return nullptr;
    }

    NodeEmitter->ResetPlacementForChain(
        EntryNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);

    return EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
}

// ---------------------------------------------------------------------------
// FindTerminalExecOutputPin
// Forward-walks the exec chain from EntryNode's first exec-output pin and
// returns the last exec-output pin that has no outgoing links — i.e. the
// splice point for Extend mode.
// Returns nullptr when the chain forks (Branch, Sequence, or any node with
// multiple exec-output pins OR multiple links on one exec-output pin).
// ---------------------------------------------------------------------------
static UEdGraphPin* FindTerminalExecOutputPin(UEdGraphNode* EntryNode)
{
    if (!EntryNode) return nullptr;

    // Find the first exec-output pin on the entry node.
    UEdGraphPin* CurrentExecOut = nullptr;
    for (UEdGraphPin* Pin : EntryNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            CurrentExecOut = Pin;
            break;
        }
    }
    if (!CurrentExecOut) return nullptr;

    // Safety bound: one pass per node in the graph.
    const int32 SafetyBound = EntryNode->GetGraph() ? EntryNode->GetGraph()->Nodes.Num() : 512;
    int32 Steps = 0;

    while (Steps++ < SafetyBound)
    {
        // Detect fork: multiple exec-output pins on this node
        int32 ExecOutCount = 0;
        for (const UEdGraphPin* Pin : CurrentExecOut->GetOwningNode()->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                ++ExecOutCount;
            }
        }
        if (ExecOutCount > 1)
        {
            return nullptr; // Forked exec (Branch/Sequence) — cannot extend linearly
        }

        // No outgoing link on the exec-output pin → terminal
        if (CurrentExecOut->LinkedTo.Num() == 0)
        {
            return CurrentExecOut;
        }

        // More than one outgoing link is also a fork
        if (CurrentExecOut->LinkedTo.Num() > 1)
        {
            return nullptr;
        }

        // Follow the single link: go to the linked node's exec input, then its exec output
        UEdGraphPin* LinkedInput = CurrentExecOut->LinkedTo[0];
        if (!LinkedInput) return nullptr;
        UEdGraphNode* NextNode = LinkedInput->GetOwningNode();
        if (!NextNode) return nullptr;

        // Find the exec-output pin on the next node
        UEdGraphPin* NextExecOut = nullptr;
        for (UEdGraphPin* Pin : NextNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                NextExecOut = Pin;
                break;
            }
        }
        if (!NextExecOut)
        {
            // Terminal node with no exec output (e.g. Return node) — this IS the end
            return CurrentExecOut;
        }

        CurrentExecOut = NextExecOut;
    }

    // Safety bound exhausted — treat as ambiguous
    return nullptr;
}

UEdGraphPin* FBpirCompiler::SetupOverride(
    const FString& Name,
    const TArray<FBpirEntryBlock::FParam>& Params,
    const FBpirTypeSpec& ReturnType,
    const TArray<FBpirEntryBlock::FParam>& OutputParams,
    EBpirCompileMode Mode,
    const FBpirEntryMetadata& Metadata)
{
    if (!TargetBlueprint)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupOverride: no blueprint"));
        return nullptr;
    }

    BlueprintHandlerUtils::FBlueprintOverrideInfo OverrideInfo;
    FString OverrideError;
    // Class-aware mapping: keep the literal name when the parent class already
    // exposes a matching UFunction (e.g. UUserWidget::Tick) and only apply the
    // Receive-prefix shorthand on AActor subclasses.
    const FName ResolvedNameFN = ResolveOverrideEventName(Name, TargetBlueprint ? TargetBlueprint->ParentClass : nullptr);
    FString ResolvedName = ResolvedNameFN.ToString();
    if (!BlueprintHandlerUtils::TryResolveBlueprintOverride(
        TargetBlueprint,
        ResolvedName,
        OverrideInfo,
        OverrideError))
    {
        AccumulatedErrors.Add(FCompileError(-1, OverrideError));
        return nullptr;
    }

    if (Params.Num() > 0 || !ReturnType.IsEmpty() || OutputParams.Num() > 0)
    {
        FBpirEntryBlock RequestedSignatureBlock;
        RequestedSignatureBlock.Kind = EBpirEntryKind::Override;
        RequestedSignatureBlock.Name = Name;
        RequestedSignatureBlock.ReturnType = ReturnType;
        RequestedSignatureBlock.Params = Params;
        RequestedSignatureBlock.OutputParams = OutputParams;

        TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ExpectedInputs;
        TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ExpectedOutputs;
        FString SignatureBuildError;
        if (!BuildExpectedOverrideSignature(
            RequestedSignatureBlock,
            ExpectedInputs,
            ExpectedOutputs,
            SignatureBuildError))
        {
            AccumulatedErrors.Add(FCompileError(-1, SignatureBuildError));
            return nullptr;
        }

        TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ActualInputs;
        TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ActualOutputs;
        FString ActualSignatureError;
        if (!BlueprintHandlerUtils::GetFunctionSignatureDescriptors(
            OverrideInfo.Function,
            ActualInputs,
            ActualOutputs,
            ActualSignatureError))
        {
            AccumulatedErrors.Add(FCompileError(-1, ActualSignatureError));
            return nullptr;
        }

        FString MismatchMessage;
        if (!BlueprintHandlerUtils::DoPinTypeDescriptorsMatch(
            ExpectedInputs,
            ExpectedOutputs,
            ActualInputs,
            ActualOutputs,
            MismatchMessage))
        {
            AccumulatedErrors.Add(FCompileError(
                -1,
                FString::Printf(
                    TEXT("Override '%s' does not match the parent signature: %s"),
                    *Name,
                    *MismatchMessage)));
            return nullptr;
        }
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(TargetBlueprint);
    if (OverrideInfo.bCanPlaceAsEvent && EventGraph)
    {
        SwitchToGraph(EventGraph);

        // Mirror Phase 0's lookup: find any UK2Node_Event across all ubergraph pages whose
        // EventReference member name matches case-insensitively. FBlueprintEditorUtils::
        // FindOverrideForFunction requires an exact OverrideClass match, which fails when
        // the existing entry stores an ancestor class (e.g. an intermediate subclass) in
        // its EventReference. The looser name-based search matches what Phase 0 deletes,
        // so Extend mode and Default mode see the same set of "existing entries".
        UK2Node_Event* EventNode = nullptr;
        const FName OverrideFnName = OverrideInfo.Function->GetFName();
        for (UEdGraph* UberGraph : TargetBlueprint->UbergraphPages)
        {
            if (!UberGraph) continue;
            for (UEdGraphNode* Node : UberGraph->Nodes)
            {
                UK2Node_Event* Candidate = Cast<UK2Node_Event>(Node);
                if (!Candidate || !Candidate->bOverrideFunction) continue;
                if (Candidate->EventReference.GetMemberName() == OverrideFnName)
                {
                    EventNode = Candidate;
                    break;
                }
            }
            if (EventNode) break;
        }
        const bool bExistedBefore = (EventNode != nullptr);
        if (!EventNode)
        {
            EventNode = NewObject<UK2Node_Event>(EventGraph);
            EventNode->EventReference.SetExternalMember(
                OverrideInfo.Function->GetFName(),
                OverrideInfo.OverrideClass);
            EventNode->bOverrideFunction = true;
            EventNode->CreateNewGuid();
            EventNode->PostPlacedNewNode();
            EventNode->AllocateDefaultPins();
            EventGraph->AddNode(EventNode, true, false);
            EventNode->NodePosX = 0;
            EventNode->NodePosY = NodeEmitter->FindFreeYPositionForEventNode();
            NodeEmitter->GetCreatedNodeGUIDs().Add(EventNode->NodeGuid);
        }

        for (UEdGraphPin* Pin : EventNode->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate
                || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
            {
                continue;
            }
            PinResolver->RegisterVariable(Pin->PinName.ToString(), Pin);
        }

        // Extend mode: if the entry already existed, splice at the terminal exec pin.
        if (Mode == EBpirCompileMode::Extend && bExistedBefore)
        {
            UEdGraphPin* TerminalPin = FindTerminalExecOutputPin(EventNode);
            if (!TerminalPin)
            {
                AccumulatedErrors.Add(FCompileError(
                    -1,
                    FString::Printf(
                        TEXT("Extend mode: override '%s' has forked or ambiguous exec flow — cannot extend linearly."),
                        *Name)));
                return nullptr;
            }
            // If the terminal already has outgoing links (e.g., to a FunctionResult node),
            // save them so we can reconnect after the new body is wired.
            if (TerminalPin->LinkedTo.Num() > 0)
            {
                FExtendReconnect Reconnect;
                Reconnect.SplicePin = TerminalPin;
                Reconnect.DownstreamPins = TerminalPin->LinkedTo;
                PendingExtendReconnects.Add(Reconnect);
                // The K2 schema TryCreateConnection will break the existing link when the
                // new body's first exec-in is wired here, so we must restore it afterward.
            }
            NodeEmitter->ResetPlacementForChain(TerminalPin, 0);
            return TerminalPin;
        }

        NodeEmitter->ResetPlacementForChain(EventNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);
        return EventNode->FindPin(UEdGraphSchema_K2::PN_Then);
    }

    UEdGraph* FuncGraph = BlueprintHandlerUtils::FindFunctionGraphByName(TargetBlueprint, Name);
    if (FuncGraph && IsAnimGraphFamily(FuncGraph))
    {
        AccumulatedErrors.Add(FCompileError(
            -1,
            FString::Printf(
                TEXT("BPIR_ANIMGRAPH_REFUSED: '%s' resolves to an anim-family graph; ")
                TEXT("BPIR cannot compile anim graphs — use AGIR (anim.compile_agir) instead."),
                *Name)));
        return nullptr;
    }
    const bool bFuncGraphExistedBefore = (FuncGraph != nullptr);
    if (!FuncGraph)
    {
        FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
            TargetBlueprint,
            FName(*Name),
            UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        if (!FuncGraph)
        {
            UE_LOG(LogBpirCompiler, Warning, TEXT("SetupOverride: CreateNewGraph failed for '%s'"), *Name);
            return nullptr;
        }

        FBlueprintEditorUtils::AddFunctionGraph(
            TargetBlueprint,
            FuncGraph,
            /*bIsUserCreated=*/false,
            OverrideInfo.OverrideClass);
        CreatedFunctionGraphs.Add(FuncGraph);
    }

    SwitchToGraph(FuncGraph);

    UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(FuncGraph);
    if (!EntryNode)
    {
        EntryNode = NodeEmitter->GetOrCreateFunctionEntry(FuncGraph);
    }
    if (!EntryNode)
    {
        AccumulatedErrors.Add(FCompileError(
            -1,
            FString::Printf(TEXT("Failed to create override entry for '%s'"), *Name)));
        return nullptr;
    }

    for (UEdGraphPin* Pin : EntryNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            PinResolver->RegisterVariable(Pin->PinName.ToString(), Pin);
        }
    }

    ApplyEntryMetadataToFunctionEntry(EntryNode, FuncGraph, Metadata);

    // Extend mode: if the function graph already existed, splice at the terminal exec pin.
    if (Mode == EBpirCompileMode::Extend && bFuncGraphExistedBefore)
    {
        UEdGraphPin* TerminalPin = FindTerminalExecOutputPin(EntryNode);
        if (!TerminalPin)
        {
            AccumulatedErrors.Add(FCompileError(
                -1,
                FString::Printf(
                    TEXT("Extend mode: override '%s' has forked or ambiguous exec flow — cannot extend linearly."),
                    *Name)));
            return nullptr;
        }
        // If the terminal already has outgoing links (e.g., to a FunctionResult node),
        // save them so we can reconnect after the new body is wired.
        if (TerminalPin->LinkedTo.Num() > 0)
        {
            FExtendReconnect Reconnect;
            Reconnect.SplicePin = TerminalPin;
            Reconnect.DownstreamPins = TerminalPin->LinkedTo;
            PendingExtendReconnects.Add(Reconnect);
        }
        NodeEmitter->ResetPlacementForChain(TerminalPin, 0);
        return TerminalPin;
    }

    NodeEmitter->ResetPlacementForChain(EntryNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);
    return EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
}

UEdGraphPin* FBpirCompiler::SetupFunction(
    const FString& Name, const FBpirTypeSpec& ReturnType, const TArray<FBpirEntryBlock::FParam>& Params,
    const TArray<FBpirEntryBlock::FParam>& OutputParams, const FBpirEntryMetadata& Metadata)
{
    if (!TargetBlueprint)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupFunction: no blueprint"));
        return nullptr;
    }

    // Create a new function graph
    UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
        TargetBlueprint,
        FName(*Name),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());

    if (!FuncGraph)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupFunction: CreateNewGraph failed for '%s'"), *Name);
        return nullptr;
    }

    FBlueprintEditorUtils::AddFunctionGraph(TargetBlueprint, FuncGraph,
        /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));
    CreatedFunctionGraphs.Add(FuncGraph);

    // Switch compiler context to the new function graph
    SwitchToGraph(FuncGraph);

    UK2Node_FunctionEntry* EntryNode = NodeEmitter->GetOrCreateFunctionEntry(FuncGraph);
    if (!EntryNode)
    {
        UE_LOG(LogBpirCompiler, Warning,
            TEXT("SetupFunction: could not get/create FunctionEntry for '%s'"), *Name);
        return nullptr;
    }

    // Add user-defined input pins for each parameter
    for (const FBpirEntryBlock::FParam& Param : Params)
    {
        if (Param.Name.IsEmpty()) { continue; }

        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        const bool bConverted = FCodePinResolver::ConvertTypeSpecToPinType(Param.Type, PinType);
        const bool bIsDelegateKind = (Param.Type.Kind == EBpirTypeKind::Delegate
            || Param.Type.Kind == EBpirTypeKind::McDelegate);

        if (!bConverted && bIsDelegateKind)
        {
            // Engine CreatePropertyOnScope returns null if the delegate's
            // PinSubCategoryMemberReference doesn't resolve; emitting the pin anyway
            // produces the engine error "Failed to create property X from <None>".
            AccumulatedErrors.Add(FCompileError(-1,
                FString::Printf(
                    TEXT("Could not resolve delegate signature '%s' on owner '%s' for parameter '%s'"),
                    *Param.Type.SecondaryInnerName.ToString(),
                    *Param.Type.InnerName.ToString(),
                    *Param.Name)));
            continue;
        }

        EntryNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
    }

    // Record the declared return type so caller-side CallFunction emission can patch
    // the ReturnValue pin's PinSubCategoryObject when the skeleton hasn't yet propagated it.
    if (!ReturnType.IsEmpty() && !ReturnType.IsVoid())
    {
        BpirDeclaredReturnTypes.Add(FName(*Name), ReturnType);
    }

    // Add return value pin on the result node if function has a non-void return type
    if (!ReturnType.IsEmpty() && !ReturnType.IsVoid())
    {
        UK2Node_FunctionResult* ResultNode = NodeEmitter->GetOrCreateFunctionResult(FuncGraph);
        if (ResultNode)
        {
            FEdGraphPinType RetPinType;
            RetPinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
            FCodePinResolver::ConvertTypeSpecToPinType(ReturnType, RetPinType);
            ResultNode->CreateUserDefinedPin(FName(TEXT("ReturnValue")), RetPinType, EGPD_Input);
            ResultNode->ReconstructNode();
        }
    }

    // Handle multi-output function: -> (type Name, type Name)
    if (OutputParams.Num() > 0)
    {
        UK2Node_FunctionResult* ResultNode = NodeEmitter->GetOrCreateFunctionResult(FuncGraph);
        if (ResultNode)
        {
            for (const FBpirEntryBlock::FParam& OutParam : OutputParams)
            {
                if (OutParam.Name.IsEmpty()) continue;
                FEdGraphPinType PinType;
                PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
                FCodePinResolver::ConvertTypeSpecToPinType(OutParam.Type, PinType);
                ResultNode->CreateUserDefinedPin(FName(*OutParam.Name), PinType, EGPD_Input);
            }
            ResultNode->ReconstructNode();
        }
    }

    ApplyEntryMetadataToFunctionEntry(EntryNode, FuncGraph, Metadata);

    EntryNode->ReconstructNode();

    // Register parameter output pins in PinResolver so function body can reference them
    for (const FBpirEntryBlock::FParam& Param : Params)
    {
        if (Param.Name.IsEmpty()) { continue; }
        UEdGraphPin* ParamPin = EntryNode->FindPin(*Param.Name);
        if (ParamPin)
        {
            PinResolver->RegisterVariable(Param.Name, ParamPin);
        }
    }

    NodeEmitter->ResetPlacementForChain(
        EntryNode->FindPin(UEdGraphSchema_K2::PN_Then), 0);

    return EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
}

UEdGraphPin* FBpirCompiler::SetupMacro(
    const FString& Name,
    const TArray<FBpirEntryBlock::FParam>& InputParams,
    const TArray<FBpirEntryBlock::FParam>& OutputParams,
    const TArray<FString>& ExecOutputNames,
    const FBpirEntryBlock& Block)
{
    if (!TargetBlueprint)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupMacro: no blueprint"));
        return nullptr;
    }

    const bool bReuseExistingMacroGraph = CompileMode == EBpirCompileMode::Replace;
    UEdGraph* MacroGraph = bReuseExistingMacroGraph
        ? BpirCompilerMacroUtils::FindMacroGraphByName(TargetBlueprint, FName(*Name))
        : nullptr;
    const bool bReusedMacroGraph = MacroGraph != nullptr;
    if (!MacroGraph)
    {
        MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
            TargetBlueprint,
            FName(*Name),
            UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());

        if (!MacroGraph)
        {
            UE_LOG(LogBpirCompiler, Warning, TEXT("SetupMacro: CreateNewGraph failed for '%s'"), *Name);
            return nullptr;
        }

        FBlueprintEditorUtils::AddMacroGraph(TargetBlueprint, MacroGraph,
            /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));
        CreatedMacroGraphs.Add(MacroGraph);
    }
    else
    {
        ReusedMacroGraphs.AddUnique(MacroGraph);
    }

    // Switch compiler context
    SwitchToGraph(MacroGraph);

    // Find the auto-created tunnel pair
    UK2Node_Tunnel* EntryTunnel = nullptr;
    UK2Node_Tunnel* ExitTunnel = nullptr;
    BlueprintHandlerUtils::FindMacroTunnelPair(MacroGraph, EntryTunnel, ExitTunnel);

    if (!EntryTunnel || !ExitTunnel)
    {
        UE_LOG(LogBpirCompiler, Warning, TEXT("SetupMacro: could not find tunnel pair for '%s'"), *Name);
        return nullptr;
    }

    if (bReusedMacroGraph)
    {
        CaptureReusedMacroGraphSnapshot(ReusedMacroGraphSnapshots, MacroGraph);
        ClearMacroBodyPreservingTunnels(TargetBlueprint, MacroGraph, EntryTunnel, ExitTunnel);
        ClearMacroTunnelPins(EntryTunnel);
        ClearMacroTunnelPins(ExitTunnel);
    }

    CurrentMacroExitTunnel = ExitTunnel;

    if (!bReusedMacroGraph)
    {
        NodeEmitter->GetCreatedNodeGUIDs().Add(EntryTunnel->NodeGuid);
        NodeEmitter->GetCreatedNodeGUIDs().Add(ExitTunnel->NodeGuid);
    }

    // Determine if macro is impure by scanning instructions
    bool bIsImpure = false;
    for (const FBpirInstruction& Inst : Block.Instructions)
    {
        if (Inst.IsImpure())
        {
            bIsImpure = true;
            break;
        }
    }

    // Add exec pins if impure
    if (bIsImpure)
    {
        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        EntryTunnel->CreateUserDefinedPin(FName(TEXT("execute")), ExecPinType, EGPD_Output);

        if (ExecOutputNames.Num() > 1)
        {
            // Multi-exit: create named exec input pins on exit tunnel
            for (const FString& ExecName : ExecOutputNames)
            {
                ExitTunnel->CreateUserDefinedPin(FName(*ExecName), ExecPinType, EGPD_Input);
            }
        }
        else
        {
            ExitTunnel->CreateUserDefinedPin(FName(TEXT("execute")), ExecPinType, EGPD_Input);
        }
    }

    // Add input parameter pins to entry tunnel (as outputs — pin mirroring)
    for (const FBpirEntryBlock::FParam& Param : InputParams)
    {
        if (Param.Name.IsEmpty()) continue;
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        FCodePinResolver::ConvertTypeSpecToPinType(Param.Type, PinType);
        EntryTunnel->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
    }

    // Add output parameter pins to exit tunnel (as inputs — pin mirroring)
    for (const FBpirEntryBlock::FParam& Param : OutputParams)
    {
        if (Param.Name.IsEmpty()) continue;
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        FCodePinResolver::ConvertTypeSpecToPinType(Param.Type, PinType);
        ExitTunnel->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Input);
    }

    ApplyEntryMetadataToMacroTunnel(EntryTunnel, Block.Metadata);

    // Reconstruct to finalize pin layout
    EntryTunnel->ReconstructNode();
    ExitTunnel->ReconstructNode();

    // Register input params in PinResolver for body $var references
    for (const FBpirEntryBlock::FParam& Param : InputParams)
    {
        if (Param.Name.IsEmpty()) continue;
        UEdGraphPin* ParamPin = EntryTunnel->FindPin(*Param.Name, EGPD_Output);
        if (ParamPin)
        {
            PinResolver->RegisterVariable(Param.Name, ParamPin);
        }
    }

    // Position and return
    UEdGraphPin* EntryExecPin = bIsImpure
        ? EntryTunnel->FindPin(FName(TEXT("execute")), EGPD_Output)
        : nullptr;

    NodeEmitter->ResetPlacementForChain(EntryExecPin, 0);

    return EntryExecPin;
}

// ----------------------------------------------------------------------------
// EmitInstruction() — Pass 2 opcode dispatch
// ----------------------------------------------------------------------------

bool FBpirCompiler::EmitInstruction(int32 InstructionIndex, FBpirInstruction& Inst, FBpirEntryBlock& Block, UEdGraphPin*& InOutExecPin)
{
    FEmittedNodeInfo& Emit = GetOrCreateEmitInfo(InstructionIndex);

    switch (Inst.Opcode)
    {
    // Pass 2b wires exec pins inline via PlaceNode() as each Call/Pure/Latent node is placed.
    // Pass 3b (WireExecPins) re-attempts exec wiring for the full instruction sequence, but the
    // LinkedTo.Num()==0 guard in WireExecPins prevents double-wiring any pins already connected
    // here. This dual-pass design is intentional: Pass 2b handles the straightforward case where
    // exec flow is known at placement time, while Pass 3b stitches together chains that depend on
    // label resolution or control-flow nodes whose final targets are only known after all nodes exist.
    case EBpirOpcode::Call:
    case EBpirOpcode::Pure:
    case EBpirOpcode::Latent:
    {
        // UK2Node_FormatText: special pure node with dynamic argument pins from format string.
        // Must be checked before function resolution because BroadSearch can find unrelated
        // UFunctions named "Format", shadowing the K2Node handler.
        if (Inst.FunctionName.Equals(TEXT("Format"), ESearchCase::IgnoreCase)
            || Inst.FunctionName.Equals(TEXT("Format_Text"), ESearchCase::IgnoreCase)
            || Inst.FunctionName.Equals(TEXT("FormatText"), ESearchCase::IgnoreCase))
        {
            FString FormatString;
            for (const FBpirArg& Arg : Inst.Args)
            {
                if (Arg.PinName.Equals(TEXT("Format"), ESearchCase::IgnoreCase))
                {
                    FormatString = FBpirValueResolver::IsLiteral(Arg.Value)
                        ? FBpirValueResolver::GetLiteralText(Arg.Value)
                        : Arg.Value;
                    break;
                }
            }
            if (!FormatString.IsEmpty())
            {
                FString FormatError;
                UK2Node_FormatText* FormatNode = NodeEmitter->CreateFormatTextNode(FormatString, &FormatError);
                if (!FormatNode)
                {
                    AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                        FormatError.IsEmpty()
                            ? TEXT("Failed to create UK2Node_FormatText node")
                            : FormatError));
                    return false;
                }

                Emit.Node = FormatNode;
                Emit.PrimaryOutputPin = FormatNode->FindPin(FName(TEXT("Result")), EGPD_Output);

                // Wire argument pins directly using FName lookup (avoids FNAME_Find issues)
                for (const FBpirArg& Arg : Inst.Args)
                {
                    if (Arg.PinName.Equals(TEXT("Format"), ESearchCase::IgnoreCase))
                    {
                        continue;
                    }
                    UEdGraphPin* ArgPin = FormatNode->FindPin(FName(*Arg.PinName));
                    if (!ArgPin)
                    {
                        for (UEdGraphPin* Pin : FormatNode->Pins)
                        {
                            if (Pin->Direction == EGPD_Input
                                && Pin->PinName.ToString().Equals(Arg.PinName, ESearchCase::IgnoreCase))
                            {
                                ArgPin = Pin;
                                break;
                            }
                        }
                    }
                    if (ArgPin)
                    {
                        UEdGraphPin* SourcePin = ValueResolver->ResolveValue(Arg.Value, Block);
                        if (SourcePin)
                        {
                            const UEdGraphSchema* Schema = ArgPin->GetSchema();
                            if (Schema) Schema->TryCreateConnection(SourcePin, ArgPin);
                        }
                        else if (FBpirValueResolver::IsLiteral(Arg.Value))
                        {
                            FString LiteralText = FBpirValueResolver::GetLiteralText(Arg.Value);
                            ArgPin->DefaultValue = LiteralText;
                        }
                    }
                    else
                    {
                        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                            FString::Printf(TEXT("Format argument '%s' not found — check that format string contains {%s}"),
                                *Arg.PinName, *Arg.PinName)));
                    }
                }
                Emit.bSkipWireDataPins = true;
                return true;
            }
        }

        // Expand node registry: specialized K2Nodes (CreateWidget, SpawnActor, etc.)
        // that produce typed output pins instead of generic UObject*/UUserWidget*.
        if (UClass* ExpandNodeClass = NodeEmitter->FindExpandNodeClass(Inst.FunctionName))
        {
            // Extract the Class: argument value
            FString ClassPath;
            for (const FBpirArg& Arg : Inst.Args)
            {
                if (Arg.PinName.Equals(TEXT("Class"), ESearchCase::IgnoreCase))
                {
                    ClassPath = FBpirValueResolver::IsLiteral(Arg.Value)
                        ? FBpirValueResolver::GetLiteralText(Arg.Value)
                        : Arg.Value;
                    break;
                }
            }

            UK2Node* ExpandNode = NodeEmitter->CreateExpandNode(ExpandNodeClass, ClassPath, InOutExecPin);
            if (ExpandNode)
            {
                Emit.Node = ExpandNode;

                // Find typed ReturnValue output pin
                Emit.PrimaryOutputPin = ExpandNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
                if (!Emit.PrimaryOutputPin)
                {
                    // Fallback: first non-exec output pin
                    for (UEdGraphPin* Pin : ExpandNode->Pins)
                    {
                        if (Pin->Direction == EGPD_Output
                            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                        {
                            Emit.PrimaryOutputPin = Pin;
                            break;
                        }
                    }
                }

                // Find exec output pin
                for (UEdGraphPin* Pin : ExpandNode->Pins)
                {
                    if (Pin->Direction == EGPD_Output
                        && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        Emit.ExecOutputPin = Pin;
                        break;
                    }
                }

                return true;
            }
            // If CreateExpandNode failed, fall through to normal function resolution
        }

        // Resolve the target function
        UClass* BPClass = TargetBlueprint->GeneratedClass
            ? TargetBlueprint->GeneratedClass
            : TargetBlueprint->ParentClass;

        auto EmitAsyncActionFactoryNode = [&](UFunction* FactoryFunc, UClass* DedicatedNodeClass) -> bool
        {
            UK2Node_AsyncAction* AsyncNode = NodeEmitter->CreateAsyncActionNode(FactoryFunc, InOutExecPin, DedicatedNodeClass);
            Emit.Node = AsyncNode;
            Emit.ExecOutputPin = InOutExecPin;
            if (AsyncNode)
            {
                Emit.PrimaryOutputPin = AsyncNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
                if (!Emit.PrimaryOutputPin)
                {
                    for (UEdGraphPin* Pin : AsyncNode->Pins)
                    {
                        if (Pin->Direction == EGPD_Output
                            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                        {
                            Emit.PrimaryOutputPin = Pin;
                            break;
                        }
                    }
                }
            }
            return Emit.Node != nullptr;
        };

        // Try Class, then its SkeletonGeneratedClass if Class is BP-generated.
        // Mirrors the resolution order ResolveFuncOnTargetClass uses for explicit targets,
        // factored out so the qualified-call short-circuit doesn't re-derive it.
        auto ResolveFunctionOnClassAndSkeleton = [&](UClass* Class, const FString& Name) -> UFunction*
        {
            UFunction* F = FunctionResolver->ResolveFunction(Class, Name);
            if (!F)
            {
                if (UBlueprint* ClassBP = Cast<UBlueprint>(Class->ClassGeneratedBy))
                {
                    if (ClassBP->SkeletonGeneratedClass && ClassBP->SkeletonGeneratedClass != Class)
                    {
                        F = FunctionResolver->ResolveFunction(ClassBP->SkeletonGeneratedClass, Name);
                    }
                }
            }
            return F;
        };

        // Helper: resolve function on a target class, with BP GeneratedClass/SkeletonClass fallback
        auto ResolveFuncOnTargetClass = [&](UClass* TargetClass) -> UFunction*
        {
            UFunction* F = FunctionResolver->ResolveFunction(TargetClass, Inst.FunctionName);
            if (!F)
            {
                if (UBlueprint* TargetBP = Cast<UBlueprint>(TargetClass->ClassGeneratedBy))
                {
                    if (TargetBP->GeneratedClass && TargetBP->GeneratedClass != TargetClass)
                    {
                        F = FunctionResolver->ResolveFunction(TargetBP->GeneratedClass, Inst.FunctionName);
                    }
                    if (!F && TargetBP->SkeletonGeneratedClass && TargetBP->SkeletonGeneratedClass != TargetClass)
                    {
                        F = FunctionResolver->ResolveFunction(TargetBP->SkeletonGeneratedClass, Inst.FunctionName);
                    }
                }
            }
            return F;
        };

        // Step 4-first: target-type-aware resolution runs before Step 1 (self-class)
        // when the call has an explicit `Target:` arg. Mirrors UE's BP node-creation
        // scoping: the source-pin type fixes the function-picker scope. Self-class
        // remains the fallback when the target class has no matching function. The
        // %ref branch keeps its inline pin-type extraction including the
        // BpirDeclaredReturnTypes fallback for wildcard ReturnValue pins.
        auto ResolveViaTargetArg = [&]() -> UFunction*
        {
            for (const FBpirArg& Arg : Inst.Args)
            {
                if (!Arg.PinName.Equals(TEXT("Target"), ESearchCase::IgnoreCase))
                {
                    continue;
                }

                if (Arg.Value.StartsWith(TEXT("%")))
                {
                    FString RefName = Arg.Value.Mid(1);
                    FString PinSuffix;
                    int32 DotIdx = INDEX_NONE;
                    if (RefName.FindChar(TEXT('.'), DotIdx))
                    {
                        PinSuffix = RefName.Mid(DotIdx + 1);
                        RefName = RefName.Left(DotIdx);
                    }
                    if (const int32* RefIdx = Block.ValueIndex.Find(RefName))
                    {
                        if (const FEmittedNodeInfo* RefEmit = EmitMap.Find(*RefIdx))
                        {
                            UEdGraphPin* ResolvedPin = RefEmit->PrimaryOutputPin;
                            if (!PinSuffix.IsEmpty() && RefEmit->Node)
                            {
                                for (UEdGraphPin* Pin : RefEmit->Node->Pins)
                                {
                                    if (Pin->Direction == EGPD_Output && Pin->PinName.ToString().Equals(PinSuffix, ESearchCase::IgnoreCase))
                                    {
                                        ResolvedPin = Pin;
                                        break;
                                    }
                                }
                            }
                            UClass* TargetClass = nullptr;
                            if (ResolvedPin)
                            {
                                TargetClass = Cast<UClass>(ResolvedPin->PinType.PinSubCategoryObject.Get());
                            }
                            // BpirDeclaredReturnTypes fallback recovers the declared return class
                            // when the producer's CallFunction node has a wildcard ReturnValue pin
                            // (skeleton UFunction generated without specialized object<T> class).
                            if (!TargetClass && Block.Instructions.IsValidIndex(*RefIdx))
                            {
                                const FBpirInstruction& SourceInst = Block.Instructions[*RefIdx];
                                if (SourceInst.Opcode == EBpirOpcode::Call
                                    || SourceInst.Opcode == EBpirOpcode::Pure
                                    || SourceInst.Opcode == EBpirOpcode::Latent)
                                {
                                    if (const FBpirTypeSpec* DeclaredRet =
                                            BpirDeclaredReturnTypes.Find(FName(*SourceInst.FunctionName)))
                                    {
                                        if (DeclaredRet->Kind == EBpirTypeKind::Object
                                            || DeclaredRet->Kind == EBpirTypeKind::Class
                                            || DeclaredRet->Kind == EBpirTypeKind::SoftObject
                                            || DeclaredRet->Kind == EBpirTypeKind::SoftClass
                                            || DeclaredRet->Kind == EBpirTypeKind::Interface)
                                        {
                                            if (!DeclaredRet->InnerName.IsNone())
                                            {
                                                TargetClass = ResolveUClass(DeclaredRet->InnerName.ToString());
                                            }
                                        }
                                    }
                                }
                            }
                            if (TargetClass)
                            {
                                return ResolveFuncOnTargetClass(TargetClass);
                            }
                        }
                    }
                }
                else if (Arg.Value.StartsWith(TEXT("$")))
                {
                    if (UClass* TargetClass = ResolveTargetClass(Arg.Value, Block))
                    {
                        return ResolveFuncOnTargetClass(TargetClass);
                    }
                }
                break; // Only process first Target arg
            }
            return nullptr;
        };

        UFunction* Func = nullptr;

        // CASCADE-COUPLING NOTE: ShouldQualifyFunctionName in
        // Decompiler/BpirTextEmitter.cpp re-implements this cascade's resolution
        // order to predict when a call site needs `Class::Method` qualification on
        // round-trip. Any reorder or new step here must also update that probe,
        // otherwise the decompiler silently under- or over-qualifies.
        //
        // Parent calls are deliberately resolved against the direct parent
        // context. Do this before the ordinary call cascade so a same-named
        // function on the Blueprint itself cannot swallow the parent intent.
        if (Inst.bParentCall)
        {
            UClass* ParentClass = Inst.TypeArg.IsEmpty()
                ? (TargetBlueprint->ParentClass ? TargetBlueprint->ParentClass.Get() : (BPClass ? BPClass->GetSuperClass() : nullptr))
                : ResolveUClass(Inst.TypeArg);
            if (!ParentClass)
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Unresolved parent class '%s' for parent call '%s'"),
                        *Inst.TypeArg, *Inst.FunctionName)));
                return false;
            }
            Func = ResolveFunctionOnClassAndSkeleton(ParentClass, Inst.FunctionName);
            if (!Func)
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Function '%s' not found on parent class '%s'"),
                        *Inst.FunctionName, *ParentClass->GetName())));
                return false;
            }
        }

        // Qualified `ClassName::MethodName` short-circuit. Explicit class qualification
        // is an explicit demand — the cascade is bypassed entirely, and an unresolved
        // method is a hard error rather than a hint to widen the search.
        if (!Inst.bParentCall && !Inst.TypeArg.IsEmpty()
            && (Inst.Opcode == EBpirOpcode::Call || Inst.Opcode == EBpirOpcode::Pure))
        {
            UClass* QualifiedClass = ResolveUClass(Inst.TypeArg);
            if (!QualifiedClass)
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Unresolved class '%s' in qualified call '%s::%s'"),
                        *Inst.TypeArg, *Inst.TypeArg, *Inst.FunctionName)));
                return false;
            }
            Func = ResolveFunctionOnClassAndSkeleton(QualifiedClass, Inst.FunctionName);
            if (!Func)
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Function '%s' not found on class '%s' (qualified call '%s::%s')"),
                        *Inst.FunctionName, *Inst.TypeArg, *Inst.TypeArg, *Inst.FunctionName)));
                return false;
            }
        }

        if (!Func)
        {
            Func = ResolveViaTargetArg();
        }

        if (!Func)
        {
            Func = FunctionResolver->ResolveFunction(BPClass, Inst.FunctionName);
        }
        // Fallback: check SkeletonGeneratedClass for names registered during Phase 1.5 skeleton regen.
        // Covers two cases:
        //   1. BPIR-declared symbols (CreatedCustomEvents, CreatedFunctionGraphs, CreatedMacroGraphs)
        //      whose UFunction only exists on SkeletonGeneratedClass until full compile.
        //   2. Pre-existing user function / macro graphs on the target BP that the BPIR code calls
        //      by their graph name (e.g. spaced names like `Set Error`). GeneratedClass lookups
        //      can fail for these before full compile; SkeletonGeneratedClass has them after Phase 1.5.
        if (!Func && TargetBlueprint->SkeletonGeneratedClass)
        {
            const FName TargetName(*Inst.FunctionName);
            auto MatchesGraphName = [&](const UEdGraph* G) { return G && G->GetFName() == TargetName; };
            const bool bIsLocalSymbol =
                CreatedCustomEvents.Contains(Inst.FunctionName)
                || CreatedFunctionGraphs.ContainsByPredicate(MatchesGraphName)
                || CreatedMacroGraphs.ContainsByPredicate(MatchesGraphName)
                || TargetBlueprint->FunctionGraphs.ContainsByPredicate(MatchesGraphName)
                || TargetBlueprint->MacroGraphs.ContainsByPredicate(MatchesGraphName);
            if (bIsLocalSymbol)
            {
                Func = FunctionResolver->ResolveFunction(TargetBlueprint->SkeletonGeneratedClass, Inst.FunctionName);
            }
        }
        if (!Func)
        {
            Func = FunctionResolver->ResolveFunctionAcrossLibraries(Inst.FunctionName);
        }
        if (!Func)
        {
            Func = FunctionResolver->ResolveFunctionBroadSearch(Inst.FunctionName);
        }
        if (!Func)
        {
            if (Inst.FunctionName.StartsWith(TEXT("K2Node_"))
                || Inst.FunctionName.StartsWith(TEXT("UK2Node_")))
            {
                return EmitGenericK2NodeInstruction(Inst.FunctionName, Inst, Emit, InOutExecPin, EmitAsyncActionFactoryNode);
            }

            FString Diag = FunctionResolver->GetSearchDiagnostic(BPClass);
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Unresolved function: '%s'. Searched:\n%sHint: If this is a member function of another class, pass the object as 'Target: %%ref' or 'Target: $var'. If it's a K2Node, use the full class name (e.g., K2Node_FormatText)."),
                    *Inst.FunctionName, *Diag)));
            return false;
        }

        // Probe the dedicated-subclass lane (HasDedicatedAsyncNode) before the generic lane.
        // The generic IsAsyncActionFactory predicate explicitly opts out when the owner class
        // carries the meta, so dedicated-subclass factories like ListenForGameplayMessages
        // only reach the async-action emitter through this probe.
        if (UClass* DedicatedSubclass = FCodeNodeEmitter::ResolveDedicatedAsyncActionSubclass(Func))
        {
            return EmitAsyncActionFactoryNode(Func, DedicatedSubclass);
        }

        if (FCodeNodeEmitter::IsAsyncActionFactory(Func))
        {
            return EmitAsyncActionFactoryNode(Func, nullptr);
        }

        // `message` only means something for a function declared on a Blueprint Interface —
        // UK2Node_Message::ExpandNode casts the self object to the interface and skips the
        // call when the cast fails. Pointed at an ordinary class function the node would be
        // a permanent no-op, so refuse it here rather than emit one that never dispatches.
        if (Inst.bInterfaceMessage)
        {
            const UClass* OwnerClass = Func->GetOuterUClass();
            if (!OwnerClass || !OwnerClass->HasAnyClassFlags(CLASS_Interface))
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine, FString::Printf(
                    TEXT("'message %s' resolved to '%s' on '%s', which is not a Blueprint Interface. "
                         "Use 'message Interface::Function(...)' to name the interface explicitly, or 'call' for an ordinary function."),
                    *Inst.FunctionName,
                    *Func->GetName(),
                    OwnerClass ? *OwnerClass->GetName() : TEXT("<unknown class>"))));
                return false;
            }
        }

        if (Inst.Opcode == EBpirOpcode::Pure)
        {
            // Pure calls: no exec pin threading
            UEdGraphPin* DummyExec = nullptr;
            UK2Node_CallFunction* CallNode = Inst.bParentCall
                ? NodeEmitter->CreateCallParentFunctionNode(Func, DummyExec)
                : NodeEmitter->CreateCallFunctionNode(Func, DummyExec, Inst.bInterfaceMessage);
            Emit.Node = CallNode;

            if (CallNode)
            {
                Emit.PrimaryOutputPin = CallNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
                if (!Emit.PrimaryOutputPin)
                {
                    // Fallback: first non-exec output pin
                    for (UEdGraphPin* Pin : CallNode->Pins)
                    {
                        if (Pin->Direction == EGPD_Output
                            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                        {
                            Emit.PrimaryOutputPin = Pin;
                            break;
                        }
                    }
                }

                Emit.PrimaryOutputPin = EnsureCallReturnPinFromBpirDeclaration(
                    CallNode, Emit.PrimaryOutputPin, Inst.FunctionName);

                // Auto-promote to call if the resolved function is not actually BlueprintPure.
                // A non-pure function will have exec pins that need wiring, otherwise the node
                // is orphaned (unreachable in the exec chain).
                if (!Func->HasAnyFunctionFlags(FUNC_BlueprintPure))
                {
                    UE_LOG(LogBpirCompiler, Warning,
                        TEXT("Line %d: Function '%s' is not BlueprintPure, auto-promoting to call with exec wiring"),
                        Inst.SourceLine, *Func->GetName());

                    // Wire exec pins the same way the Call case does
                    if (InOutExecPin)
                    {
                        UEdGraphPin* ExecInputPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
                        if (ExecInputPin)
                        {
                            const UEdGraphSchema* Schema = InOutExecPin->GetSchema();
                            if (Schema)
                            {
                                Schema->TryCreateConnection(InOutExecPin, ExecInputPin);
                            }
                        }
                    }
                    UEdGraphPin* ThenPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
                    if (ThenPin)
                    {
                        InOutExecPin = ThenPin;
                        Emit.ExecOutputPin = ThenPin;
                    }
                }
            }
        }
        else
        {
            // Impure (Call or Latent): thread exec pin.
            // Save exec pin in case the function turns out to be pure at runtime.
            UEdGraphPin* SavedExecPin = InOutExecPin;
            UK2Node_CallFunction* CallNode = Inst.bParentCall
                ? NodeEmitter->CreateCallParentFunctionNode(Func, InOutExecPin)
                : NodeEmitter->CreateCallFunctionNode(Func, InOutExecPin, Inst.bInterfaceMessage);
            Emit.Node = CallNode;
            Emit.ExecOutputPin = InOutExecPin;

            RestoreExecIfPure(CallNode, InOutExecPin, SavedExecPin, Emit, Inst);

            if (CallNode)
            {
                Emit.PrimaryOutputPin = CallNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
                if (!Emit.PrimaryOutputPin)
                {
                    for (UEdGraphPin* Pin : CallNode->Pins)
                    {
                        if (Pin->Direction == EGPD_Output
                            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                        {
                            Emit.PrimaryOutputPin = Pin;
                            break;
                        }
                    }
                }

                Emit.PrimaryOutputPin = EnsureCallReturnPinFromBpirDeclaration(
                    CallNode, Emit.PrimaryOutputPin, Inst.FunctionName);
            }
        }
        return Emit.Node != nullptr;
    }

    case EBpirOpcode::Subsystem:
    {
        // Resolve subsystem class from TypeArg
        UClass* SubsystemClass = ResolveUClass(Inst.TypeArg);
        if (!SubsystemClass)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Unresolved subsystem class: %s"), *Inst.TypeArg)));
            return false;
        }

        UK2Node_GetSubsystem* SubsystemNode = NodeEmitter->CreateGetSubsystemNode(SubsystemClass);
        Emit.Node = SubsystemNode;
        if (SubsystemNode)
        {
            Emit.PrimaryOutputPin = SubsystemNode->GetResultPin();
        }
        return SubsystemNode != nullptr;
    }

    case EBpirOpcode::Branch:
    {
        UK2Node_IfThenElse* BranchNode = NodeEmitter->CreateBranchNode(InOutExecPin);
        Emit.Node = BranchNode;
        // Branch has multiple exec outputs handled by label-based wiring; don't set ExecOutputPin
        return BranchNode != nullptr;
    }

    case EBpirOpcode::Foreach:
    {
        UK2Node_MacroInstance* Node = NodeEmitter->CreateMacroNode(FName(BpirSharedConstants::MacroNames::ForEachLoop), InOutExecPin);
        Emit.Node = Node;
        if (Node)
        {
            Emit.PrimaryOutputPin = Node->FindPin(TEXT("Array Element"));
            if (!Emit.PrimaryOutputPin)
            {
                Emit.PrimaryOutputPin = Node->FindPin(TEXT("Element"));
            }
        }
        return Node != nullptr;
    }

    case EBpirOpcode::ForeachBreak:
    {
        UK2Node_MacroInstance* Node = NodeEmitter->CreateMacroNode(FName(BpirSharedConstants::MacroNames::ForEachLoopWithBreak), InOutExecPin);
        Emit.Node = Node;
        if (Node)
        {
            Emit.PrimaryOutputPin = Node->FindPin(TEXT("Array Element"));
            if (!Emit.PrimaryOutputPin)
            {
                Emit.PrimaryOutputPin = Node->FindPin(TEXT("Element"));
            }
        }
        return Node != nullptr;
    }

    case EBpirOpcode::While:
    {
        UK2Node_MacroInstance* Node = NodeEmitter->CreateMacroNode(FName(BpirSharedConstants::MacroNames::While), InOutExecPin);
        Emit.Node = Node;
        return Node != nullptr;
    }

    case EBpirOpcode::Switch:
    {
        UK2Node_SwitchName* SwitchNode = NodeEmitter->CreateSwitchNameNode(InOutExecPin);
        Emit.Node = SwitchNode;
        if (SwitchNode)
        {
            // Add case output exec pins from the exec targets
            for (const FBpirExecTarget& Target : Inst.ExecTargets)
            {
                if (!Target.PinName.Equals(TEXT("default"), ESearchCase::IgnoreCase))
                {
                    SwitchNode->PinNames.Add(FName(*Target.PinName));
                    SwitchNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, FName(*Target.PinName));
                }
            }
        }
        return SwitchNode != nullptr;
    }

    case EBpirOpcode::SwitchInt:
    {
        // Resolve the case labels before the node exists, so a label set the
        // engine would renumber on load is rejected instead of leaving a
        // half-built node behind.
        TArray<int32> CaseValues;
        FString CaseError;
        if (!ResolveSwitchIntCaseValues(Inst.ExecTargets, CaseValues, CaseError))
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine, CaseError));
            return false;
        }

        UK2Node_SwitchInteger* SwitchNode = NodeEmitter->CreateSwitchIntegerNode(InOutExecPin);
        Emit.Node = SwitchNode;
        if (SwitchNode)
        {
            // Anchor StartIndex to the lowest case and add the case pins in
            // ascending order. ReallocatePinsDuringReconstruction then renumbers
            // them to exactly the names they already carry, so the labels survive
            // compile-on-load instead of shifting. Wiring is by pin name
            // (FindExecOutputPin), so the pin order need not follow the BPIR text.
            if (CaseValues.Num() > 0)
            {
                SwitchNode->StartIndex = CaseValues[0];
            }
            for (const int32 CaseValue : CaseValues)
            {
                SwitchNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,
                    FName(*FString::FromInt(CaseValue)));
            }
        }
        return SwitchNode != nullptr;
    }

    case EBpirOpcode::SwitchString:
    {
        UK2Node_SwitchString* SwitchNode = NodeEmitter->CreateSwitchStringNode(InOutExecPin);
        Emit.Node = SwitchNode;
        if (SwitchNode)
        {
            // Add case output exec pins from the exec targets, stripping quotes from pin names
            for (const FBpirExecTarget& Target : Inst.ExecTargets)
            {
                if (!Target.PinName.Equals(TEXT("default"), ESearchCase::IgnoreCase))
                {
                    FString CaseName = Target.PinName;
                    // Strip surrounding quotes — parser stores "hello" but UE pin name is hello
                    if (CaseName.Len() >= 2 && CaseName.StartsWith(TEXT("\"")) && CaseName.EndsWith(TEXT("\"")))
                    {
                        CaseName = CaseName.Mid(1, CaseName.Len() - 2);
                    }
                    SwitchNode->PinNames.Add(FName(*CaseName));
                    SwitchNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, FName(*CaseName));
                }
            }
        }
        return SwitchNode != nullptr;
    }

    case EBpirOpcode::SwitchEnum:
    {
        UEnum* EnumType = ResolveUEnum(Inst.TypeArg);
        if (!EnumType)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Unresolved enum for switch: %s"), *Inst.TypeArg)));
            return false;
        }

        UK2Node_SwitchEnum* SwitchNode = NodeEmitter->CreateSwitchEnumNode(EnumType, InOutExecPin);
        Emit.Node = SwitchNode;
        return SwitchNode != nullptr;
    }

    case EBpirOpcode::Sequence:
    {
        UK2Node_ExecutionSequence* Node = NodeEmitter->CreateSequenceNode(Inst.SequenceCount, InOutExecPin);
        Emit.Node = Node;
        return Node != nullptr;
    }

    case EBpirOpcode::Cast:
    {
        // Resolve target class from TypeArg via the unified resolver.
        // Handles short names (W_Error_C), /Script/Module.Class, and
        // content-mount BP paths (/Game/Path/BP.BP_C, plugin mounts) — including bare
        // short names like "W_Error_C" that FindFirstObjectSafe misses
        // for unloaded Blueprint generated classes.
        UClass* TargetClass = ResolveUClass(Inst.TypeArg);
        if (!TargetClass)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Unresolved class for cast: %s"), *Inst.TypeArg)));
            return false;
        }

        UK2Node_DynamicCast* CastNode = NodeEmitter->CreateCastNode(TargetClass, InOutExecPin);
        Emit.Node = CastNode;
        if (CastNode)
        {
            // Primary output is "As <ClassName>" pin
            FString AsPin = FString::Printf(TEXT("As %s"), *Inst.TypeArg);
            Emit.PrimaryOutputPin = CastNode->FindPin(*AsPin);
            if (!Emit.PrimaryOutputPin)
            {
                // Fallback: first non-exec, non-bool output
                for (UEdGraphPin* Pin : CastNode->Pins)
                {
                    if (Pin->Direction == EGPD_Output
                        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Boolean)
                    {
                        Emit.PrimaryOutputPin = Pin;
                        break;
                    }
                }
            }
        }
        return CastNode != nullptr;
    }

    case EBpirOpcode::Select:
    {
        UEdGraphPin* DummyExec = nullptr;
        UEnum* IndexEnum = ResolveSelectIndexEnum(Inst, Block, ValueResolver.Get());
        UK2Node_Select* Node = NodeEmitter->CreateSelectNode(DummyExec, IndexEnum);
        Emit.Node = Node;
        if (Node)
        {
            // Type the option pins before Pass 3a applies their literals, so an
            // FText option default reaches DefaultTextValue instead of the
            // DefaultValue slot a PC_Text pin never reads.
            FEdGraphPinType SelectResultType;
            if (ResolveSelectResultPinType(Inst, SelectResultType))
            {
                PreTypeSelectPins(Node, SelectResultType);
            }
            Emit.PrimaryOutputPin = Node->GetReturnValuePin();
        }
        return Node != nullptr;
    }

    case EBpirOpcode::Macro:
    {
        UK2Node_MacroInstance* Node = NodeEmitter->CreateMacroNode(FName(*Inst.FunctionName), InOutExecPin);
        Emit.Node = Node;
        if (!Node)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Macro '%s' not found in StandardMacros or target Blueprint MacroGraphs"), *Inst.FunctionName)));
            return false;
        }
        return true;
    }

    case EBpirOpcode::Timeline:
    {
        UK2Node_Timeline* Node = NodeEmitter->CreateTimelineNode(Inst.FunctionName, InOutExecPin);
        Emit.Node = Node;
        if (Node)
        {
            // Add float tracks from args (e.g., Alpha: float_curve(...))
            UTimelineTemplate* TmplObj = TargetBlueprint->Timelines.Num() > 0
                ? TargetBlueprint->Timelines.Last()
                : nullptr;
            if (TmplObj)
            {
                for (const FBpirArg& Arg : Inst.Args)
                {
                    if (!Arg.PinName.IsEmpty() && Arg.Value.StartsWith(TEXT("float_curve")))
                    {
                        FTTFloatTrack NewTrack;
                        NewTrack.SetTrackName(FName(*Arg.PinName), TmplObj);
                        NewTrack.CurveFloat = NewObject<UCurveFloat>(TmplObj);
                        ParseFloatCurveKeyframes(Arg.Value, NewTrack.CurveFloat);
                        TmplObj->FloatTracks.Add(NewTrack);
                    }
                }
                // Refresh pins to include new track output pins
                Node->ReconstructNode();
            }
        }
        return Node != nullptr;
    }

    case EBpirOpcode::Set:
    {
        FString VarName = Inst.FunctionName;
        UK2Node_VariableSet* SetNode = nullptr;

        if (!Inst.TypeArg.IsEmpty())
        {
            // External set: resolve target class from TypeArg ($target or %ref)
            UClass* TargetClass = ResolveTargetClass(Inst.TypeArg, Block);
            if (TargetClass)
            {
                SetNode = NodeEmitter->CreateExternalVariableSetNode(FName(*VarName), TargetClass, InOutExecPin);
            }
            else
            {
                FString Reason;
                if (Inst.TypeArg.StartsWith(TEXT("$")))
                {
                    FString TargetVarName;
                    FString PropertyName;
                    SplitDollarReference(Inst.TypeArg.Mid(1), TargetVarName, PropertyName);
                    UEdGraphPin* Pin = PinResolver->ResolveVariable(TargetVarName);
                    if (!Pin)
                    {
                        Reason = FString::Printf(TEXT("Variable '%s' not found. For widget variables, ensure 'Is Variable' is checked in the Designer panel."), *TargetVarName);
                    }
                    else
                    {
                        UClass* PinClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
                        if (!PinClass)
                        {
                            Reason = FString::Printf(TEXT("Variable '%s' resolved to pin type '%s' which is not an object class."), *TargetVarName, *Pin->PinType.PinCategory.ToString());
                        }
                    }
                }
                else if (Inst.TypeArg.StartsWith(TEXT("%")))
                {
                    Reason = FString::Printf(TEXT("Reference '%s' not found in emitted nodes. Check that the referenced instruction has a result name."), *Inst.TypeArg);
                }
                if (Reason.IsEmpty())
                {
                    Reason = TEXT("Target could not be resolved to an object class.");
                }
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Cannot set '%s.%s': %s"), *Inst.TypeArg, *VarName, *Reason)));
                return false;
            }
        }
        else
        {
            SetNode = NodeEmitter->CreateVariableSetNode(FName(*VarName), InOutExecPin);
        }

        Emit.Node = SetNode;
        Emit.ExecOutputPin = InOutExecPin;
        return SetNode != nullptr;
    }

    case EBpirOpcode::Get:
    {
        UK2Node_VariableGet* GetNode = NodeEmitter->CreateVariableGetNode(FName(*Inst.FunctionName));
        Emit.Node = GetNode;
        if (GetNode)
        {
            for (UEdGraphPin* Pin : GetNode->Pins)
            {
                if (Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    Emit.PrimaryOutputPin = Pin;
                    break;
                }
            }
        }
        return GetNode != nullptr;
    }

    case EBpirOpcode::BreakStruct:
    {
        UScriptStruct* Struct = ResolveUScriptStruct(Inst.TypeArg);
        if (!Struct)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Unresolved struct for break: %s"), *Inst.TypeArg)));
            return false;
        }

        UK2Node_BreakStruct* Node = NodeEmitter->CreateBreakStructNode(Struct);
        Emit.Node = Node;
        return Node != nullptr;
    }

    case EBpirOpcode::MakeStruct:
    {
        UScriptStruct* Struct = ResolveUScriptStruct(Inst.TypeArg);
        if (!Struct)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Unresolved struct for make: %s"), *Inst.TypeArg)));
            return false;
        }

        // Routing policy:
        // - Bare-name form (e.g. `make<Vector>`) where TypeArg matches the struct's
        //   UObject name exactly: if the struct has HasNativeMake metadata, route to
        //   the native CallFunction (preserves long-standing behavior — UE itself
        //   does this for Vector/Rotator/etc.).
        // - F-prefixed form (e.g. `make<FVector>`) where TypeArg was F-stripped to
        //   resolve: honor the literal opcode and emit UK2Node_MakeStruct. Authors
        //   writing the C++-style `FVector` are typically asking for the literal
        //   MakeStruct node.
        const bool bIsBareNameForm = Struct->GetName().Equals(Inst.TypeArg, ESearchCase::CaseSensitive);
        if (bIsBareNameForm && Struct->HasMetaData(TEXT("HasNativeMake")))
        {
            const FString NativeMakePath = Struct->GetMetaData(TEXT("HasNativeMake"));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            UFunction* NativeFunc = FindObject<UFunction>(nullptr, *NativeMakePath, EFindObjectFlags::ExactClass);
#else
            UFunction* NativeFunc = FindObject<UFunction>(nullptr, *NativeMakePath, true);
#endif
            if (!NativeFunc)
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Struct '%s' has HasNativeMake metadata but native function '%s' could not be resolved"),
                        *Inst.TypeArg, *NativeMakePath)));
                return false;
            }

            // MakeStruct CallFunction is pure (IsImpure()==false), so WireExecPins skips it.
            // Restore InOutExecPin since CreateCallFunctionNode nulls it for pure functions.
            UEdGraphPin* SavedExecPin = InOutExecPin;
            UK2Node_CallFunction* CallNode = NodeEmitter->CreateCallFunctionNode(NativeFunc, InOutExecPin);
            Emit.Node = CallNode;
            InOutExecPin = SavedExecPin;
            if (CallNode)
            {
                Emit.PrimaryOutputPin = CallNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
            }
            return CallNode != nullptr;
        }

        UK2Node_MakeStruct* Node = NodeEmitter->CreateMakeStructNode(Struct);
        Emit.Node = Node;
        if (Node)
        {
            // Primary output is the struct output pin
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    Emit.PrimaryOutputPin = Pin;
                    break;
                }
            }
        }
        return Node != nullptr;
    }

    case EBpirOpcode::MakeArray:
    {
        UK2Node_MakeArray* Node = NodeEmitter->CreateMakeArrayNode(Inst.Args.Num());
        Emit.Node = Node;
        if (Node)
        {
            // Type the element pins before Pass 3a applies their literals, so an
            // FText element default reaches DefaultTextValue instead of the
            // DefaultValue slot a PC_Text pin never reads.
            FEdGraphPinType ArrayPinType;
            if (ResolveMakeArrayOutputPinType(Inst, ArrayPinType))
            {
                PreTypeMakeArrayPins(Node, ArrayPinType);
            }

            // Primary output is the array output
            Emit.PrimaryOutputPin = Node->FindPin(TEXT("Array"));
            if (!Emit.PrimaryOutputPin)
            {
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin->Direction == EGPD_Output)
                    {
                        Emit.PrimaryOutputPin = Pin;
                        break;
                    }
                }
            }
        }
        return Node != nullptr;
    }

    case EBpirOpcode::Self:
    {
        UK2Node_Self* SelfNode = NodeEmitter->CreateSelfNode();
        Emit.Node = SelfNode;
        if (SelfNode)
        {
            for (UEdGraphPin* Pin : SelfNode->Pins)
            {
                if (Pin->Direction == EGPD_Output)
                {
                    Emit.PrimaryOutputPin = Pin;
                    break;
                }
            }
        }
        return SelfNode != nullptr;
    }

    case EBpirOpcode::Enum:
    {
        // Enum literals are handled at the pin level during wiring.
        // No node is created — the literal text is set directly on target pins.
        // Emit.Node remains nullptr.
        return true;
    }

    case EBpirOpcode::Return:
    {
        // Check if we're in a macro graph (use exit tunnel instead of FunctionResult)
        bool bIsMacro = (CurrentMacroExitTunnel != nullptr);

        if (bIsMacro)
        {
            UK2Node_Tunnel* ExitTunnel = CurrentMacroExitTunnel;
            if (!ExitTunnel)
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine, TEXT("No exit tunnel found for macro return")));
                return false;
            }
            Emit.Node = ExitTunnel;
        }
        else
        {
            // Function context: every `return` statement owns its own UK2Node_FunctionResult.
            // That is the engine's model — several result nodes per function graph are legal and
            // FKCHandler_FunctionResult merges them — and it is what a human authors when each
            // branch ends in its own Return node. Sharing one node across returns is what dropped
            // every branch but the last: a result node's data inputs take a single link each, so
            // the later return's TryCreateConnection silently unwired the earlier branch's value
            // and left its producers orphaned (B-bpir-second-return-branch-data-dropped).
            //
            // Exec pins are wired in Pass 3b, AFTER every node in the block is emitted, so the
            // exec-in LinkedTo test below cannot tell a genuinely free result node from one an
            // earlier `return` in this block already claimed. EmitMap is reset per block and only
            // `return` ever stores a FunctionResult in it, so it is the authoritative claim record.
            auto IsClaimedByEarlierReturn = [this](const UEdGraphNode* Candidate) -> bool
            {
                for (const TPair<int32, FEmittedNodeInfo>& Entry : EmitMap)
                {
                    if (Entry.Value.Node == Candidate)
                    {
                        return true;
                    }
                }
                return false;
            };

            UK2Node_FunctionResult* ResultNode = nullptr;
            {
                UK2Node_FunctionResult* Existing = nullptr;
                for (UEdGraphNode* N : CurrentGraph->Nodes)
                {
                    UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N);
                    if (!R || IsClaimedByEarlierReturn(R))
                    {
                        continue;
                    }
                    UEdGraphPin* ExecIn = R->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
                    if (ExecIn && ExecIn->LinkedTo.Num() == 0)
                    {
                        Existing = R;
                        break;
                    }
                }
                ResultNode = Existing ? Existing : NodeEmitter->CreateFunctionResult(CurrentGraph);
            }
            if (!ResultNode)
            {
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine, TEXT("No function result node available for return")));
                return false;
            }
            Emit.Node = ResultNode;
        }
        return true;
    }

    case EBpirOpcode::End:
    case EBpirOpcode::ExecGoto:
    case EBpirOpcode::Label:
    case EBpirOpcode::Comment:
        // These don't create nodes — handled during wiring or ignored
        return true;

    case EBpirOpcode::Alias:
    {
        // No graph node is created. Resolution is deferred to ResolvePercentRef.
        // The VariableGet for a $var RHS is pre-emitted by PreEmitVariableRefs,
        // but only for aliases that are actually referenced downstream — dead
        // aliases (no reader) don't need a backing VariableGet, so we don't
        // emit one here either.
        Emit.Node = nullptr;
        Emit.PrimaryOutputPin = nullptr;
        return true;
    }

    case EBpirOpcode::CallDispatcher:
    case EBpirOpcode::BindDispatcher:
    case EBpirOpcode::UnbindDispatcher:
    case EBpirOpcode::ClearDispatcher:
    {
#if BPIR_HAS_DELEGATE_NODES
        // Find the delegate property — check external target first, fall back to self
        FName DelegateName(*Inst.FunctionName);

        // Pre-scan for Target and Delegate args (avoids repeated linear scans in the lambda)
        FString TargetArgValue;
        FString DelegateArgValue;
        for (const FBpirArg& Arg : Inst.Args)
        {
            // BPIR keyword `event:` maps to the K2 pin `Delegate`; `target:` matches `Target` via case-folding.
            if (TargetArgValue.IsEmpty() && Arg.PinName.Equals(TEXT("Target"), ESearchCase::IgnoreCase))
            {
                TargetArgValue = Arg.Value;
            }
            else if (DelegateArgValue.IsEmpty()
                && (Arg.PinName.Equals(TEXT("Delegate"), ESearchCase::IgnoreCase)
                    || Arg.PinName.Equals(TEXT("event"), ESearchCase::IgnoreCase)))
            {
                DelegateArgValue = Arg.Value;
            }
        }

        auto CreateDelegateNode = [&](auto* NodeTemplate) -> bool
        {
            using NodeType = typename TRemovePointer<decltype(NodeTemplate)>::Type;
            NodeType* DelegateNode = NewObject<NodeType>(CurrentGraph);

            // Determine target class: external target or self
            UClass* SearchClass = TargetBlueprint->GeneratedClass;
            bool bSelfContext = true;
            if (!TargetArgValue.IsEmpty())
            {
                UClass* ExtClass = ResolveTargetClass(TargetArgValue, Block);
                if (ExtClass)
                {
                    SearchClass = ExtClass;
                    bSelfContext = false;
                }
            }

            FProperty* DelegateProp = SearchClass
                ? SearchClass->FindPropertyByName(DelegateName)
                : nullptr;
            if (!DelegateProp)
            {
                UE_LOG(LogBpirCompiler, Warning, TEXT("Delegate property '%s' not found on class '%s'"),
                    *DelegateName.ToString(),
                    SearchClass ? *SearchClass->GetName() : TEXT("null"));
                DelegateNode->MarkAsGarbage();
                return false;
            }
            DelegateNode->SetFromProperty(DelegateProp, bSelfContext, SearchClass);
            DelegateNode->CreateNewGuid();
            DelegateNode->PostPlacedNewNode();
            DelegateNode->AllocateDefaultPins();
            CurrentGraph->AddNode(DelegateNode, true, false);
            NodeEmitter->GetCreatedNodeGUIDs().Add(DelegateNode->NodeGuid);

            Emit.Node = DelegateNode;

            // Wire Target/Self pin for external targets
            UEdGraphPin* TargetSourcePin = nullptr;
            if (!bSelfContext)
            {
                TargetSourcePin = ValueResolver->ResolveValue(TargetArgValue, Block);
                UEdGraphPin* SelfPin = DelegateNode->FindPin(UEdGraphSchema_K2::PN_Self);
                if (SelfPin && TargetSourcePin)
                {
                    const UEdGraphSchema* Schema = SelfPin->GetSchema();
                    if (Schema) Schema->TryCreateConnection(TargetSourcePin, SelfPin);
                }
            }

            // Wire exec
            if (InOutExecPin)
            {
                UEdGraphPin* ExecIn = nullptr;
                for (UEdGraphPin* Pin : DelegateNode->Pins)
                {
                    if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        ExecIn = Pin;
                        break;
                    }
                }
                if (ExecIn)
                {
                    const UEdGraphSchema* Schema = InOutExecPin->GetSchema();
                    if (Schema)
                    {
                        if (!Schema->TryCreateConnection(InOutExecPin, ExecIn))
                        {
                            UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: TryCreateConnection failed wiring exec '%s' -> '%s' on delegate node"),
                                Inst.SourceLine, *InOutExecPin->PinName.ToString(), *ExecIn->PinName.ToString());
                            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                                FString::Printf(TEXT("TryCreateConnection failed wiring exec '%s' -> '%s' on delegate node"),
                                    *InOutExecPin->PinName.ToString(), *ExecIn->PinName.ToString())));
                        }
                    }
                }
            }

            for (UEdGraphPin* Pin : DelegateNode->Pins)
            {
                if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    InOutExecPin = Pin;
                    Emit.ExecOutputPin = Pin;
                    break;
                }
            }

            // %ref values are left for the generic pin wiring pass; bare event names get a CreateDelegate node.
            if (!DelegateArgValue.IsEmpty() && !DelegateArgValue.StartsWith(TEXT("%")))
            {
                // Find the Delegate input pin on this bind node, then delegate to the shared helper.
                UEdGraphPin* DelegateInPin = nullptr;
                for (UEdGraphPin* Pin : DelegateNode->Pins)
                {
                    if (Pin->Direction == EGPD_Input
                        && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate
                            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate))
                    {
                        DelegateInPin = Pin;
                        break;
                    }
                }
                WireCreateDelegateForBindNode(DelegateInPin, DelegateArgValue, bSelfContext, Block);
            }

            return true;
        };

        bool bCreated = false;
        if (Inst.Opcode == EBpirOpcode::CallDispatcher)
        {
            bCreated = CreateDelegateNode(static_cast<UK2Node_CallDelegate*>(nullptr));
        }
        else if (Inst.Opcode == EBpirOpcode::BindDispatcher)
        {
            bCreated = CreateDelegateNode(static_cast<UK2Node_AddDelegate*>(nullptr));
        }
        else if (Inst.Opcode == EBpirOpcode::UnbindDispatcher)
        {
            bCreated = CreateDelegateNode(static_cast<UK2Node_RemoveDelegate*>(nullptr));
        }
        else if (Inst.Opcode == EBpirOpcode::ClearDispatcher)
        {
            bCreated = CreateDelegateNode(static_cast<UK2Node_ClearDelegate*>(nullptr));
        }

        // Skip generic data pin wiring — Target and Delegate args are handled
        // in the emit pass above (CreateDelegateNode lambda + K2Node_CreateDelegate).
        // call_dispatcher still needs generic wiring for its data args.
        if (bCreated && Inst.Opcode != EBpirOpcode::CallDispatcher)
        {
            Emit.bSkipWireDataPins = true;
        }

        if (!bCreated || !Emit.Node)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Failed to create delegate node for dispatcher: %s"), *Inst.FunctionName)));
            return false;
        }
        return true;
#else
        // Fallback: resolve as a regular function call
        UClass* BPClass = TargetBlueprint->GeneratedClass
            ? TargetBlueprint->GeneratedClass
            : TargetBlueprint->ParentClass;

        UFunction* Func = BPClass ? BPClass->FindFunctionByName(FName(*Inst.FunctionName)) : nullptr;
        if (!Func)
        {
            FString DispatcherFuncName;
            if (Inst.Opcode == EBpirOpcode::BindDispatcher)
            {
                DispatcherFuncName = FString::Printf(TEXT("%s__DelegateSignature"), *Inst.FunctionName);
            }
            else
            {
                DispatcherFuncName = Inst.FunctionName;
            }
            Func = BPClass ? BPClass->FindFunctionByName(FName(*DispatcherFuncName)) : nullptr;
        }

        if (Func)
        {
            UK2Node_CallFunction* CallNode = NodeEmitter->CreateCallFunctionNode(Func, InOutExecPin);
            Emit.Node = CallNode;
            Emit.ExecOutputPin = InOutExecPin;
        }
        else
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("Unresolved dispatcher: %s"), *Inst.FunctionName)));
            return false;
        }
        return Emit.Node != nullptr;
#endif
    }

    case EBpirOpcode::FieldNotifySubscribe:
    case EBpirOpcode::FieldNotifyUnsubscribe:
    {
#if BPIR_HAS_DELEGATE_NODES
        const FName FuncName = (Inst.Opcode == EBpirOpcode::FieldNotifySubscribe)
            ? FName(BpirSharedConstants::FieldNotify::SubscribeFnName)
            : FName(BpirSharedConstants::FieldNotify::UnsubscribeFnName);

        // Pre-scan args for Target and event (same scan as dispatcher block)
        FString TargetArgValue;
        FString EventArgValue;
        for (const FBpirArg& Arg : Inst.Args)
        {
            if (TargetArgValue.IsEmpty() && Arg.PinName.Equals(TEXT("Target"), ESearchCase::IgnoreCase))
                TargetArgValue = Arg.Value;
            else if (EventArgValue.IsEmpty()
                && (Arg.PinName.Equals(TEXT("event"), ESearchCase::IgnoreCase)
                    || Arg.PinName.Equals(TEXT("Delegate"), ESearchCase::IgnoreCase)))
                EventArgValue = Arg.Value;
        }

        // Resolve target class (external or self)
        UClass* SearchClass = TargetBlueprint->GeneratedClass;
        bool bSelfContext = true;
        if (!TargetArgValue.IsEmpty() && !TargetArgValue.Equals(TEXT("self"), ESearchCase::IgnoreCase))
        {
            UClass* ExtClass = ResolveTargetClass(TargetArgValue, Block);
            if (ExtClass)
            {
                SearchClass = ExtClass;
                bSelfContext = false;
            }
        }

        UFunction* TargetFunc = SearchClass ? SearchClass->FindFunctionByName(FuncName) : nullptr;
        if (!TargetFunc)
        {
            // Walk parent chain — UWidget may not be the direct class
            for (UClass* C = SearchClass; C && !TargetFunc; C = C->GetSuperClass())
            {
                TargetFunc = C->FindFunctionByName(FuncName);
            }
        }
        if (!TargetFunc)
        {
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("FieldNotify: function '%s' not found on '%s'"),
                    *FuncName.ToString(),
                    SearchClass ? *SearchClass->GetName() : TEXT("null"))));
            return false;
        }

        UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(CurrentGraph);
        CallNode->SetFromFunction(TargetFunc);
        CallNode->CreateNewGuid();
        CallNode->PostPlacedNewNode();
        CallNode->AllocateDefaultPins();
        CurrentGraph->AddNode(CallNode, true, false);
        NodeEmitter->GetCreatedNodeGUIDs().Add(CallNode->NodeGuid);

        Emit.Node = CallNode;

        UEdGraphPin* FieldIdPin = CallNode->FindPin(TEXT("FieldId"));
        if (FieldIdPin)
        {
            // UE serializes FFieldNotificationId as a struct literal with a single FieldName member.
            FieldIdPin->DefaultValue = FString::Printf(TEXT("(FieldName=\"%s\")"), *Inst.FunctionName);
        }

        if (!bSelfContext)
        {
            UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self);
            UEdGraphPin* TargetSourcePin = ValueResolver->ResolveValue(TargetArgValue, Block);
            if (SelfPin && TargetSourcePin)
            {
                const UEdGraphSchema* Schema = SelfPin->GetSchema();
                if (Schema) Schema->TryCreateConnection(TargetSourcePin, SelfPin);
            }
        }

        if (InOutExecPin)
        {
            UEdGraphPin* ExecIn = nullptr;
            for (UEdGraphPin* Pin : CallNode->Pins)
            {
                if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    ExecIn = Pin;
                    break;
                }
            }
            if (ExecIn)
            {
                const UEdGraphSchema* Schema = InOutExecPin->GetSchema();
                if (Schema) Schema->TryCreateConnection(InOutExecPin, ExecIn);
            }
        }

        for (UEdGraphPin* Pin : CallNode->Pins)
        {
            if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                InOutExecPin = Pin;
                Emit.ExecOutputPin = Pin;
                break;
            }
        }

        if (!EventArgValue.IsEmpty() && !EventArgValue.StartsWith(TEXT("%")))
        {
            UEdGraphPin* DelegateInPin = CallNode->FindPin(TEXT("Delegate"));
            WireCreateDelegateForBindNode(DelegateInPin, EventArgValue, bSelfContext, Block);
        }

        Emit.bSkipWireDataPins = true;
        return true;
#else
        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
            TEXT("FieldNotifySubscribe/Unsubscribe requires delegate node headers (BPIR_HAS_DELEGATE_NODES)")));
        return false;
#endif
    }

    default:
        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
            FString::Printf(TEXT("Unhandled opcode: %d"), static_cast<int32>(Inst.Opcode))));
        return false;
    }
}

// ----------------------------------------------------------------------------
// WireCreateDelegateForBindNode — shared CreateDelegate synthesis tail
// ----------------------------------------------------------------------------

void FBpirCompiler::WireCreateDelegateForBindNode(
    UEdGraphPin* DelegateInPin,
    const FString& EventArgValue,
    bool bSelfContext,
    FBpirEntryBlock& Block)
{
#if BPIR_HAS_DELEGATE_NODES
    UK2Node_CreateDelegate* CDNode = NewObject<UK2Node_CreateDelegate>(CurrentGraph);
    CDNode->CreateNewGuid();
    CDNode->PostPlacedNewNode();
    CDNode->AllocateDefaultPins();
    CurrentGraph->AddNode(CDNode, true, false);
    NodeEmitter->GetCreatedNodeGUIDs().Add(CDNode->NodeGuid);

    UEdGraphPin* DelegateOutPin = CDNode->GetDelegateOutPin();
    if (DelegateOutPin && DelegateInPin)
    {
        const UEdGraphSchema* Schema = DelegateOutPin->GetSchema();
        if (Schema) Schema->TryCreateConnection(DelegateOutPin, DelegateInPin);
    }

    // Wire self to CDNode's object pin only when the bind target is external.
    // For same-class binds, GetScopeClass() treats the unlinked self pin as
    // implicit self — identical result without a redundant K2Node_Self on the graph.
    if (!bSelfContext)
    {
        UEdGraphPin* ObjPin = CDNode->GetObjectInPin();
        if (ObjPin)
        {
            ValueResolver->PreEmitSelf();
            UEdGraphPin* SelfPin = ValueResolver->ResolveValue(TEXT("self"), Block);
            if (SelfPin)
            {
                const UEdGraphSchema* Schema = ObjPin->GetSchema();
                if (Schema) Schema->TryCreateConnection(SelfPin, ObjPin);
            }
        }
    }

    // Strip BPIR reference sigil (@Handler -> Handler) then resolve via HandleAnyChange.
    // HandleAnyChange() reads the object pin's linked class to resolve SelectedFunctionName
    // into a real UFunction reference — required for BP compiler to accept the node.
    FString FunctionName = EventArgValue;
    if (FunctionName.StartsWith(TEXT("@")))
    {
        FunctionName = FunctionName.RightChop(1);
    }
    CDNode->SetFunction(FName(*FunctionName));
    CDNode->HandleAnyChange();
#endif
}

// ----------------------------------------------------------------------------
// WireDataPins() — Pass 3: wire data pins for one instruction
// ----------------------------------------------------------------------------

bool FBpirCompiler::WireDataPins(int32 InstructionIndex, FBpirInstruction& Inst, FBpirEntryBlock& Block)
{
    const FEmittedNodeInfo& Emit = EmitMap[InstructionIndex];
    if (!Emit.Node)
    {
        return false;
    }

    bool bAllWired = true;
    UK2Node_Select* EnumSelectNode = nullptr;
    TMap<int64, UEdGraphPin*> SelectOptionPinsByEnumValue;
    if (Inst.Opcode == EBpirOpcode::Select)
    {
        UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Emit.Node);
        if (SelectNode && SelectNode->GetEnum())
        {
            EnumSelectNode = SelectNode;
            BuildSelectOptionPinsByEnumValue(EnumSelectNode, SelectOptionPinsByEnumValue);
        }
    }

    const FWireDataPinsContext PinContext{ Emit, Inst, EnumSelectNode, SelectOptionPinsByEnumValue };

    for (int32 ArgIdx = 0; ArgIdx < Inst.Args.Num(); ++ArgIdx)
    {
        const FBpirArg& Arg = Inst.Args[ArgIdx];

        // Determine which pin to wire to based on opcode and argument context
        bool bSkipArg = false;
        UEdGraphPin* TargetPin = ResolveTargetPinForOpcode(ArgIdx, Arg, PinContext, bSkipArg);
        if (bSkipArg)
        {
            continue;
        }

        if (!TargetPin)
        {
            const FString ErrorMsg = BuildMissingPinHint(Emit.Node, Arg.PinName);
            UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: %s"), Inst.SourceLine, *ErrorMsg);
            bAllWired = false;
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine, ErrorMsg));
            continue;
        }

        // Tripwire for B-bpir-second-return-branch-data-dropped. In function context each
        // `return` owns its own FunctionResult, so a return output pin already fed by a node
        // THIS compile authored means an earlier statement's value is about to be silently
        // unwired — a data input takes one link and TryCreateConnection replaces it without
        // reporting anything, which is how a two-branch function used to ship one branch's
        // values on both paths. Fail instead of succeeding with a lie. Links that pre-date the
        // compile (Extend mode splicing into an already-authored result node) are deliberately
        // left alone, and macro returns are excluded: a macro graph has exactly one exit tunnel
        // whose data pins are shared by every exit, which is the engine's own model.
        if (Inst.Opcode == EBpirOpcode::Return && Emit.Node->IsA<UK2Node_FunctionResult>()
            && TargetPin->LinkedTo.Num() > 0)
        {
            const UEdGraphPin* ExistingLink = TargetPin->LinkedTo[0];
            const UEdGraphNode* ExistingSource = ExistingLink ? ExistingLink->GetOwningNodeUnchecked() : nullptr;
            if (ExistingSource && NodeEmitter->GetCreatedNodeGUIDs().Contains(ExistingSource->NodeGuid))
            {
                const FString ErrorMsg = FString::Printf(
                    TEXT("Return output pin '%s' is already wired by an earlier statement in this ")
                    TEXT("compile — wiring '%s' would silently discard it. Give each return its own ")
                    TEXT("value, or merge the branches before returning."),
                    *TargetPin->PinName.ToString(), *Arg.Value);
                UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: %s"), Inst.SourceLine, *ErrorMsg);
                bAllWired = false;
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine, ErrorMsg));
                continue;
            }
        }

        auto ApplyDefaultValueToTargetPin = [&](const FString& DefaultValue) -> bool
        {
            FString DefaultError;
            if (FCodePinResolver::SetPinDefaultValue(TargetPin, DefaultValue, &DefaultError))
            {
                return true;
            }

            const FString ErrorMsg = DefaultError.IsEmpty()
                ? FString::Printf(TEXT("Could not set default value '%s' for pin '%s'"),
                    *DefaultValue, *Arg.PinName)
                : DefaultError;
            UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: %s"), Inst.SourceLine, *ErrorMsg);
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine, ErrorMsg));
            bAllWired = false;
            return false;
        };

        // Resolve the argument value
        UEdGraphPin* SourcePin = ValueResolver->ResolveValue(Arg.Value, Block);
        if (SourcePin)
        {
            // Wire connection between source and target
            const UEdGraphSchema* Schema = TargetPin->GetSchema();
            if (Schema)
            {
                if (!Schema->TryCreateConnection(SourcePin, TargetPin))
                {
                    // Self->self wiring is redundant (UE auto-wires self pins) — treat as warning only
                    bool bIsSelfToSelf = (SourcePin->PinName == UEdGraphSchema_K2::PN_Self
                        && TargetPin->PinName == UEdGraphSchema_K2::PN_Self);
                    UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: TryCreateConnection failed wiring data '%s' -> '%s'%s"),
                        Inst.SourceLine, *SourcePin->PinName.ToString(), *TargetPin->PinName.ToString(),
                        bIsSelfToSelf ? TEXT(" (redundant self wire, non-fatal)") : TEXT(""));
                    if (!bIsSelfToSelf)
                    {
                        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                            FString::Printf(TEXT("TryCreateConnection failed wiring data '%s' -> '%s'"),
                                *SourcePin->PinName.ToString(), *TargetPin->PinName.ToString())));
                        bAllWired = false;
                    }
                }
            }
        }
        else if (FBpirValueResolver::IsLiteral(Arg.Value))
        {
            // Set as literal default value
            FString LiteralText = FBpirValueResolver::GetLiteralText(Arg.Value);
            ApplyDefaultValueToTargetPin(LiteralText);
        }
        // Inline enum literal: EnumType::Value (e.g., EWeaponState::Reloading)
        else if (Arg.Value.Contains(TEXT("::")) && !Arg.Value.StartsWith(TEXT("%")) && !Arg.Value.StartsWith(TEXT("$")))
        {
            ApplyDefaultValueToTargetPin(Arg.Value);
        }
        // Handle enum %ref resolution — enum instructions have no emitted node
        else if (Arg.Value.StartsWith(TEXT("%")))
        {
            FString RefName = Arg.Value.Mid(1);
            // Check for dot notation (%name.Pin)
            int32 DotIdx = INDEX_NONE;
            RefName.FindChar(TEXT('.'), DotIdx);
            if (DotIdx != INDEX_NONE)
            {
                RefName = RefName.Left(DotIdx);
            }

            const int32* RefIdx = Block.ValueIndex.Find(RefName);
            if (RefIdx && Block.Instructions.IsValidIndex(*RefIdx))
            {
                const FBpirInstruction& RefInst = Block.Instructions[*RefIdx];
                if (RefInst.Opcode == EBpirOpcode::Enum && !RefInst.TypeArg.IsEmpty())
                {
                    // Pass the full enum literal (e.g. "ETextGender::Masculine") to
                    // SetPinDefaultValue so that PC_Byte/enum pins resolve numerically
                    // and string pins preserve the qualified name for round-trip.
                    ApplyDefaultValueToTargetPin(RefInst.TypeArg);
                }
                else if (RefInst.Opcode == EBpirOpcode::Alias)
                {
                    // Alias resolved to nullptr (the cycle guard in ResolvePercentRef
                    // already logged a warning for diagnostics). The surrounding call
                    // node was emitted successfully — the only fallout is this pin
                    // stays unconnected. Downgrade to a warning so atomic rollback
                    // does NOT wipe the otherwise-valid node.
                    UE_LOG(LogBpirCompiler, Warning,
                        TEXT("Line %d: Alias chain for '%s' could not be resolved — leaving pin '%s' unconnected"),
                        Inst.SourceLine, *Arg.Value, *Arg.PinName);
                }
                else
                {
                    UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: Could not resolve value '%s' for pin '%s'"),
                        Inst.SourceLine, *Arg.Value, *Arg.PinName);
                    bAllWired = false;
                    AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                        FString::Printf(TEXT("Could not resolve value '%s' for pin '%s'"), *Arg.Value, *Arg.PinName)));
                }
            }
            else
            {
                UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: Could not resolve value '%s' for pin '%s'"),
                    Inst.SourceLine, *Arg.Value, *Arg.PinName);
                bAllWired = false;
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Could not resolve value '%s' for pin '%s'"), *Arg.Value, *Arg.PinName)));
            }
        }
        else
        {
            // Delegate to the shared resolver which handles short names, /Script/Module.Class,
            // /Game/.../BP, plugin-mount BP.BP_C paths, automatic _C suffix, and UBlueprint->GeneratedClass fallback.
            bool bResolved = false;
            UClass* ResolvedClass = ResolveUClass(Arg.Value);
            if (ResolvedClass)
            {
                ApplyDefaultValueToTargetPin(ResolvedClass->GetPathName());
                bResolved = true;
            }
            if (!bResolved)
            {
                FString DiagMsg = FString::Printf(TEXT("Could not resolve value '%s' for pin '%s'"), *Arg.Value, *Arg.PinName);

                // Add diagnostic hint for $variable failures
                if (Arg.Value.StartsWith(TEXT("$")) && TargetBlueprint)
                {
                    FString VarName;
                    FString PropertyName;
                    SplitDollarReference(Arg.Value.Mid(1), VarName, PropertyName);
                    bool bIsEntryParam = false;
                    for (const FBpirEntryBlock::FParam& P : Block.Params)
                    {
                        if (P.Name == VarName) { bIsEntryParam = true; break; }
                    }

                    if (bIsEntryParam)
                    {
                        DiagMsg += FString::Printf(
                            TEXT(" — param '%s' was declared in the entry block but exposes no "
                                 "matching data pin on the delegate signature"),
                            *VarName);
                    }
                    else
                    {
                        UClass* GenClass = TargetBlueprint->GeneratedClass;
                        if (GenClass && !GenClass->FindPropertyByName(FName(*VarName)))
                        {
                            DiagMsg += FString::Printf(
                                TEXT(" — variable '%s' not found on %s (for widgets: check 'Is Variable' in Designer)"),
                                *VarName, *GenClass->GetName());
                        }
                    }
                }

                UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: %s"), Inst.SourceLine, *DiagMsg);
                bAllWired = false;
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine, DiagMsg));
            }
        }
    }

    // Wire external set/get target pins (the Self/Target pin on external variable nodes)
    if (Inst.Opcode == EBpirOpcode::Set && !Inst.TypeArg.IsEmpty())
    {
        UEdGraphPin* SelfPin = Emit.Node->FindPin(UEdGraphSchema_K2::PN_Self);
        if (SelfPin)
        {
            UEdGraphPin* TargetSourcePin = ValueResolver->ResolveValue(Inst.TypeArg, Block);
            if (TargetSourcePin)
            {
                const UEdGraphSchema* Schema = SelfPin->GetSchema();
                if (Schema)
                {
                    if (!Schema->TryCreateConnection(TargetSourcePin, SelfPin))
                    {
                        UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: TryCreateConnection failed wiring external set target '%s' -> Self"),
                            Inst.SourceLine, *TargetSourcePin->PinName.ToString());
                        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                            FString::Printf(TEXT("TryCreateConnection failed wiring external set target '%s' -> Self"),
                                *TargetSourcePin->PinName.ToString())));
                        bAllWired = false;
                    }
                }
            }
            else
            {
                UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: Could not resolve external set target '%s'"),
                    Inst.SourceLine, *Inst.TypeArg);
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Could not resolve external set target '%s'"), *Inst.TypeArg)));
                bAllWired = false;
            }
        }
    }

    return bAllWired;
}

UEdGraphPin* FBpirCompiler::ResolveTargetExecInput(const FBpirExecTarget& Target,
    FBpirEntryBlock& Block, int32 SourceLine, bool bLogOnMissingLabel, bool& bHadError)
{
    bHadError = false;
    const int32 TargetIdx = FindFirstImpureAtLabel(Target.Label, Block);
    if (TargetIdx < 0)
    {
        if (!Block.LabelIndex.Contains(Target.Label))
        {
            if (bLogOnMissingLabel)
            {
                UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: Could not find impure instruction at label '@%s'"),
                    SourceLine, *Target.Label);
            }
            AccumulatedErrors.Add(FCompileError(SourceLine,
                FString::Printf(TEXT("Could not find impure instruction at label '@%s'"), *Target.Label)));
            bHadError = true;
        }
        return nullptr;
    }

    UEdGraphPin* ExecIn = GetExecInputPin(TargetIdx, Block.Instructions[TargetIdx], Target.TargetInputPinName);
    if (!ExecIn && !Target.TargetInputPinName.IsEmpty())
    {
        ReportMissingTargetInputPin(EmitMap.Find(TargetIdx), SourceLine,
            Target.TargetInputPinName, TEXT("Target node"), AccumulatedErrors);
        bHadError = true;
    }
    return ExecIn;
}

// ----------------------------------------------------------------------------
// WireExecPins() — Pass 3: auto-chain + label-based exec wiring
// ----------------------------------------------------------------------------

bool FBpirCompiler::WireExecPins(FBpirEntryBlock& Block, UEdGraphPin* EntryExecPin)
{
    bool bHadErrors = false;

    // Step 1: Build a list of "label segments" — groups of instructions between labels.
    // The implicit first segment starts at instruction index 0 and uses EntryExecPin.
    struct FLabelSegment
    {
        FString LabelName;     // Empty for the implicit first segment
        int32 StartIndex;      // First instruction index in this segment
        int32 EndIndex;        // One past the last instruction index
    };

    TArray<FLabelSegment> Segments;

    // Build segments
    {
        FLabelSegment CurrentSegment;
        CurrentSegment.LabelName = FString();
        CurrentSegment.StartIndex = 0;

        for (int32 i = 0; i < Block.Instructions.Num(); ++i)
        {
            if (Block.Instructions[i].Opcode == EBpirOpcode::Label)
            {
                // Close current segment
                CurrentSegment.EndIndex = i;
                Segments.Add(CurrentSegment);

                // Start new segment
                CurrentSegment.LabelName = Block.Instructions[i].FunctionName;
                CurrentSegment.StartIndex = i + 1;
            }
        }

        // Close final segment
        CurrentSegment.EndIndex = Block.Instructions.Num();
        Segments.Add(CurrentSegment);
    }

    // Step 2: For each segment, auto-chain impure instructions sequentially.
    //
    // Cross-label fall-through (the documented bpir.instructions §2.8 "auto-chain to
    // the next label" idiom): a label segment whose preceding segment did NOT end in
    // a terminator falls through into this segment's first impure node. We carry the
    // previous segment's trailing exec output forward in FallThroughExec and seed each
    // segment's chain from it. The intra-segment chain below already collapses
    // LastExecOutput to nullptr at every terminator (a multi-output branch/switch node
    // at the HasExecTargets() check, an ExecGoto, a bare End, or a node with no exec output
    // such as a return/FunctionResult), so a terminated segment carries a null pin and emits NO
    // fall-through edge; only a genuinely open-ended block reconverges into the next
    // label. Without this carry every cross-label fall-through was silently dropped —
    // the block's terminal exec output was orphaned and the next label kept only its
    // explicit predecessors (B-bpir-fallthrough-reconverge-dropped).
    //
    // FallThroughExec seeds from EntryExecPin so the implicit first segment chains from
    // the entry pin; each later segment chains from the previous segment's trailing output.
    UEdGraphPin* FallThroughExec = Block.EntryExecTargets.IsEmpty() ? EntryExecPin : nullptr;
    for (FLabelSegment& Segment : Segments)
    {
        UEdGraphPin* LastExecOutput = FallThroughExec;

        for (int32 i = Segment.StartIndex; i < Segment.EndIndex; ++i)
        {
            FBpirInstruction& Inst = Block.Instructions[i];
            const FEmittedNodeInfo* EmitInfo = EmitMap.Find(i);

            // An explicit jump (exec -> @label) terminates this segment's local exec chain:
            // control is diverted to the goto target (wired in Step 4), so the segment must
            // NOT fall through to the next adjacent label. ExecGoto creates no node, so it
            // would otherwise hit the no-node `continue` below WITHOUT collapsing
            // LastExecOutput — leaving the previous impure node's exec output to be wrongly
            // consumed as a fall-through into the next label AND stolen from the ExecGoto's
            // own Step-4 source pin (the cause of the spurious "ExecGoto has no source exec
            // pin" failures). Collapse it here so a goto-terminated segment carries a null
            // fall-through, exactly like a branch/switch terminator.
            if (Inst.Opcode == EBpirOpcode::ExecGoto)
            {
                LastExecOutput = nullptr;
                continue;
            }
            if (Inst.Opcode == EBpirOpcode::End)
            {
                // `end` is node-less: clearing the carried output is its entire compile effect.
                LastExecOutput = nullptr;
                continue;
            }

            if (!Inst.IsImpure() || !EmitInfo || !EmitInfo->Node)
            {
                continue;
            }

            // Skip instructions with exec targets — they manage their own exec output wiring
            // But we still need to wire their exec INPUT from the previous chain
            UEdGraphPin* ExecIn = GetExecInputPin(i, Inst);
            // Guard prevents double-wiring — exec pins already wired by PlaceNode() in Pass 2b
            // are skipped here because their LinkedTo array is non-empty.
            if (LastExecOutput && ExecIn && ExecIn->LinkedTo.Num() == 0)
            {
                const UEdGraphSchema* Schema = LastExecOutput->GetSchema();
                if (Schema)
                {
                    if (!Schema->TryCreateConnection(LastExecOutput, ExecIn))
                    {
                        UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: TryCreateConnection failed auto-chaining exec '%s' -> '%s'"),
                            Inst.SourceLine, *LastExecOutput->PinName.ToString(), *ExecIn->PinName.ToString());
                        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                            FString::Printf(TEXT("TryCreateConnection failed auto-chaining exec '%s' -> '%s'"),
                                *LastExecOutput->PinName.ToString(), *ExecIn->PinName.ToString())));
                        bHadErrors = true;
                    }
                }
            }

            // Advance the chain if this instruction has a simple exec output
            // (instructions with exec targets like Branch, Switch, etc. break the chain)
            if (Inst.HasExecTargets())
            {
                // Don't auto-chain past multi-output nodes — their outputs go to labels
                LastExecOutput = nullptr;
            }
            else if (EmitInfo->ExecOutputPin)
            {
                LastExecOutput = EmitInfo->ExecOutputPin;
            }
            else
            {
                // Fallback for impure nodes: find the first exec output pin
                UEdGraphPin* FallbackExec = nullptr;
                for (UEdGraphPin* Pin : EmitInfo->Node->Pins)
                {
                    if (Pin->Direction == EGPD_Output
                        && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        FallbackExec = Pin;
                        break;
                    }
                }
                LastExecOutput = FallbackExec;
            }
        }

        // Carry this segment's trailing exec output into the next label segment so an
        // open-ended block falls through to the next adjacent label (see Step 2 header).
        // A segment that ended in a terminator already has LastExecOutput == nullptr,
        // which correctly suppresses the fall-through edge.
        FallThroughExec = LastExecOutput;
    }

    // Step 3: Wire explicit exec targets (instructions with ExecTargets like Branch, Switch, etc.)
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        FBpirInstruction& Inst = Block.Instructions[i];
        const FEmittedNodeInfo* EmitInfo = EmitMap.Find(i);

        if (!Inst.HasExecTargets() || !EmitInfo || !EmitInfo->Node)
        {
            continue;
        }

        for (const FBpirExecTarget& Target : Inst.ExecTargets)
        {
            if (Inst.Opcode == EBpirOpcode::SwitchEnum
                && Target.PinName.Equals(TEXT("default"), ESearchCase::IgnoreCase))
            {
                UK2Node_SwitchEnum* SwitchNode = Cast<UK2Node_SwitchEnum>(EmitInfo->Node);
                UEnum* EnumType = ResolveUEnum(Inst.TypeArg);
                if (!SwitchNode || !EnumType)
                {
                    AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                        FString::Printf(TEXT("switch_enum default arm: could not resolve enum '%s'"), *Inst.TypeArg)));
                    bHadErrors = true;
                    continue;
                }

                bool bResolveError = false;
                UEdGraphPin* DefaultTargetExecIn = ResolveTargetExecInput(Target, Block,
                    Inst.SourceLine, /*bLogOnMissingLabel=*/false, bResolveError);
                if (bResolveError)
                {
                    bHadErrors = true;
                    continue;
                }
                if (!DefaultTargetExecIn)
                {
                    // Empty convergence label — nothing to wire.
                    continue;
                }

                if (!WireSwitchEnumDefaultArm(SwitchNode, EnumType, Inst.ExecTargets,
                    DefaultTargetExecIn, Inst.SourceLine, AccumulatedErrors))
                {
                    bHadErrors = true;
                }
                continue;
            }

            // Find the named exec output pin on this instruction's node
            UEdGraphPin* ExecOutPin = FindExecOutputPin(i, Inst, Target.PinName);
            if (!ExecOutPin)
            {
                UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: Could not find exec output pin '%s' on node"),
                    Inst.SourceLine, *Target.PinName);
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(TEXT("Could not find exec output pin '%s' on node"), *Target.PinName)));
                bHadErrors = true;
                continue;
            }

            bool bResolveError = false;
            UEdGraphPin* TargetExecIn = ResolveTargetExecInput(Target, Block,
                Inst.SourceLine, /*bLogOnMissingLabel=*/true, bResolveError);
            if (bResolveError)
            {
                bHadErrors = true;
                continue;
            }
            if (!TargetExecIn)
            {
                // Empty convergence label — exec flow terminates here.
                continue;
            }
            const UEdGraphSchema* Schema = ExecOutPin->GetSchema();
            if (Schema)
            {
                const UEdGraphNode* SrcNode = ExecOutPin->GetOwningNode();
                const UEdGraphNode* DstNode = TargetExecIn->GetOwningNode();
                if (!Schema->TryCreateConnection(ExecOutPin, TargetExecIn))
                {
                    UE_LOG(LogBpirCompiler, Warning,
                        TEXT("WireExecPins.Step3: Line %d inst[%d]=%s TryCreateConnection FAILED label='@%s' src=%s(guid=%s).%s dst=%s(guid=%s).%s"),
                        Inst.SourceLine, i, BpirOpcodeName(Inst.Opcode), *Target.Label,
                        SrcNode ? *SrcNode->GetClass()->GetName() : TEXT("<null>"),
                        SrcNode ? *SrcNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                        *ExecOutPin->PinName.ToString(),
                        DstNode ? *DstNode->GetClass()->GetName() : TEXT("<null>"),
                        DstNode ? *DstNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                        *TargetExecIn->PinName.ToString());
                    AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                        FString::Printf(TEXT("TryCreateConnection failed wiring exec '%s' -> label '@%s'"),
                            *ExecOutPin->PinName.ToString(), *Target.Label)));
                    bHadErrors = true;
                }
                else
                {
                    UE_LOG(LogBpirCompiler, Verbose,
                        TEXT("WireExecPins.Step3: Line %d inst[%d]=%s TryCreateConnection OK label='@%s' src=%s(guid=%s).%s dst=%s(guid=%s).%s"),
                        Inst.SourceLine, i, BpirOpcodeName(Inst.Opcode), *Target.Label,
                        SrcNode ? *SrcNode->GetClass()->GetName() : TEXT("<null>"),
                        SrcNode ? *SrcNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                        *ExecOutPin->PinName.ToString(),
                        DstNode ? *DstNode->GetClass()->GetName() : TEXT("<null>"),
                        DstNode ? *DstNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>"),
                        *TargetExecIn->PinName.ToString());
                }
            }
        }
    }

    // Step 3b: Post-wire verification — walk every impure instruction with
    // labeled exec targets and confirm the corresponding K2Node output exec pin
    // ended up with a non-empty LinkedTo. This catches the silent miss in
    // B-bpir-statement-cast-success-unwired-replace-shared-topology where Step 3
    // saw `ResolveTargetExecInput == nullptr` (empty convergence label / pure-
    // patched first impure) and dropped the edge without raising an error.
    //
    // For each `[pin -> @label]` clause on an impure instruction, the rule is:
    //   - If FindFirstImpureAtLabel(@label) >= 0 (the label has at least one
    //     impure target), then the corresponding output exec pin on the source
    //     K2Node MUST be linked.
    //   - If the label is empty (FindFirstImpureAtLabel returns -1 but
    //     Block.LabelIndex contains it), the pin may legitimately be unlinked
    //     (exec flow terminates at the convergence point) — InsertCodeAfterNode's
    //     trailing reattach handles that case separately.
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        FBpirInstruction& Inst = Block.Instructions[i];
        const FEmittedNodeInfo* EmitInfo = EmitMap.Find(i);
        if (!Inst.HasExecTargets() || !EmitInfo || !EmitInfo->Node)
        {
            continue;
        }

        for (const FBpirExecTarget& Target : Inst.ExecTargets)
        {
            // SwitchEnum's "default" arm is wired through a helper and reads
            // remaining enum entries; skip verification (Step 3 already errors
            // explicitly on its failures).
            if (Inst.Opcode == EBpirOpcode::SwitchEnum
                && Target.PinName.Equals(TEXT("default"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            const int32 TargetIdx = FindFirstImpureAtLabel(Target.Label, Block);
            if (TargetIdx < 0)
            {
                // Empty convergence label — pin may be intentionally unwired.
                continue;
            }

            UEdGraphPin* SrcPin = FindExecOutputPin(i, Inst, Target.PinName);
            if (!SrcPin)
            {
                // Step 3 already reported missing exec output pins.
                continue;
            }

            if (SrcPin->LinkedTo.Num() == 0)
            {
                UE_LOG(LogBpirCompiler, Warning,
                    TEXT("WireExecPins.Verify: inst[%d]=%s line %d output pin '%s' (label='@%s') has zero LinkedTo after wire-up"),
                    i, BpirOpcodeName(Inst.Opcode), Inst.SourceLine,
                    *SrcPin->PinName.ToString(), *Target.Label);
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                    FString::Printf(
                        TEXT("Post-wire verification: labeled exec target '%s -> @%s' on %s was not wired (target label resolves to inst[%d])"),
                        *Target.PinName, *Target.Label, BpirOpcodeName(Inst.Opcode), TargetIdx)));
                bHadErrors = true;
            }
        }
    }

    if (Block.EntryExecTargets.Num() > 0)
    {
        UEdGraphNode* EntryNode = EntryExecPin ? EntryExecPin->GetOwningNode() : nullptr;
        for (const FBpirExecTarget& Target : Block.EntryExecTargets)
        {
            UEdGraphPin* ExecOutPin = EntryNode
                ? EntryNode->FindPin(FName(*Target.PinName), EGPD_Output)
                : nullptr;
            if (!ExecOutPin || ExecOutPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                AccumulatedErrors.Add(FCompileError(-1, FString::Printf(
                    TEXT("Could not find input_action exec output pin '%s'"), *Target.PinName)));
                bHadErrors = true;
                continue;
            }

            bool bResolveError = false;
            UEdGraphPin* TargetExecIn = ResolveTargetExecInput(
                Target, Block, -1, /*bLogOnMissingLabel=*/true, bResolveError);
            if (bResolveError)
            {
                bHadErrors = true;
                continue;
            }
            if (!TargetExecIn)
            {
                continue;
            }

            const UEdGraphSchema* Schema = ExecOutPin->GetSchema();
            if (!Schema || !Schema->TryCreateConnection(ExecOutPin, TargetExecIn))
            {
                AccumulatedErrors.Add(FCompileError(-1, FString::Printf(
                    TEXT("TryCreateConnection failed wiring input_action event '%s' -> label '@%s'"),
                    *Target.PinName, *Target.Label)));
                bHadErrors = true;
            }
        }
    }

    // Step 4: Wire ExecGoto instructions
    for (int32 i = 0; i < Block.Instructions.Num(); ++i)
    {
        FBpirInstruction& Inst = Block.Instructions[i];
        if (Inst.Opcode != EBpirOpcode::ExecGoto)
        {
            continue;
        }

        // The target label is stored in ExecTargets[0].Label, not FunctionName
        FString TargetLabel = Inst.ExecTargets.Num() > 0 ? Inst.ExecTargets[0].Label : FString();

        // Find the previous impure instruction's exec output pin — but only within this
        // ExecGoto's OWN label segment. An ExecGoto's straight-line source is the last
        // impure node since the preceding label or `end`; both are hard source boundaries.
        // A goto with no in-segment source is either an empty forwarding label
        // (`@A: exec -> @B`) or a source-less goto after `end`. In both cases there is
        // nothing to wire, and crossing the boundary would borrow an unrelated exec output.
        UEdGraphPin* SourceExecPin = nullptr;
        bool bHitSourceBoundary = false;
        for (int32 j = i - 1; j >= 0; --j)
        {
            FBpirInstruction& PrevInst = Block.Instructions[j];
            if (PrevInst.Opcode == EBpirOpcode::Label || PrevInst.Opcode == EBpirOpcode::End)
            {
                bHitSourceBoundary = true;
                break;
            }
            const FEmittedNodeInfo* PrevEmit = EmitMap.Find(j);
            if (PrevInst.IsImpure() && PrevEmit && PrevEmit->Node)
            {
                if (PrevEmit->ExecOutputPin && PrevEmit->ExecOutputPin->LinkedTo.Num() == 0)
                {
                    SourceExecPin = PrevEmit->ExecOutputPin;
                }
                else if (!PrevEmit->ExecOutputPin || PrevEmit->ExecOutputPin->LinkedTo.Num() > 0)
                {
                    // Find the first unconnected exec output
                    for (UEdGraphPin* Pin : PrevEmit->Node->Pins)
                    {
                        if (Pin->Direction == EGPD_Output
                            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                            && Pin->LinkedTo.Num() == 0)
                        {
                            SourceExecPin = Pin;
                            break;
                        }
                    }
                }
                break;
            }
        }

        if (!SourceExecPin)
        {
            if (bHitSourceBoundary)
            {
                // A source-less goto at a label or after `end` is a node-less no-op here.
                // Empty forwarding labels were already resolved to their final target.
                continue;
            }
            UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: ExecGoto has no source exec pin"), Inst.SourceLine);
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                TEXT("ExecGoto has no source exec pin")));
            bHadErrors = true;
            continue;
        }

        int32 TargetIdx = FindFirstImpureAtLabel(TargetLabel, Block);
        if (TargetIdx < 0)
        {
            // Empty convergence label (exists but has no impure instructions) = valid no-op
            if (Block.LabelIndex.Contains(TargetLabel))
            {
                continue;
            }
            // Label truly doesn't exist — this IS an error
            UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: ExecGoto target label '@%s' not found"),
                Inst.SourceLine, *TargetLabel);
            AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                FString::Printf(TEXT("ExecGoto target label '@%s' not found"), *TargetLabel)));
            bHadErrors = true;
            continue;
        }

        const FString GotoOverridePin = Inst.ExecTargets.Num() > 0 ? Inst.ExecTargets[0].TargetInputPinName : FString();
        UEdGraphPin* TargetExecIn = GetExecInputPin(TargetIdx, Block.Instructions[TargetIdx], GotoOverridePin);
        if (!TargetExecIn && !GotoOverridePin.IsEmpty())
        {
            // Hard-fail: ExecGoto named a specific input exec pin that doesn't exist on the target.
            ReportMissingTargetInputPin(EmitMap.Find(TargetIdx), Inst.SourceLine,
                GotoOverridePin, TEXT("ExecGoto target node"), AccumulatedErrors);
            bHadErrors = true;
        }
        else if (TargetExecIn)
        {
            const UEdGraphSchema* Schema = SourceExecPin->GetSchema();
            if (Schema)
            {
                if (!Schema->TryCreateConnection(SourceExecPin, TargetExecIn))
                {
                    UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: TryCreateConnection failed wiring ExecGoto -> label '@%s'"),
                        Inst.SourceLine, *TargetLabel);
                    AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                        FString::Printf(TEXT("TryCreateConnection failed wiring ExecGoto -> label '@%s'"), *TargetLabel)));
                    bHadErrors = true;
                }
            }
        }
    }

    return !bHadErrors;
}

// ----------------------------------------------------------------------------
// FindFirstImpureAtLabel()
// ----------------------------------------------------------------------------

int32 FBpirCompiler::FindFirstImpureAtLabel(const FString& LabelName, FBpirEntryBlock& Block)
{
    // Forwarding-label resolution: an empty label whose only exec content is a leading
    // `exec -> @T` (e.g. the decompiler's representation of a fall-through reconvergence
    // arm — `@done: exec -> @merge`) is a pure exec alias for @T. Follow the goto chain so
    // any exec target pointing at the alias resolves to the first impure node it ultimately
    // reaches, instead of treating the alias as a dead end. Forwarding chains are short and
    // rare, so the common non-forwarding lookup stays allocation-free: instead of a visited
    // TSet we cap the hop count at the label count — a chain cannot visit more distinct
    // labels than exist, so exceeding that count means a forwarding cycle
    // (`@a: exec -> @b` / `@b: exec -> @a`); bail to -1 (handled by callers as an empty
    // convergence label).
    FString CurrentLabel = LabelName;
    int32 ForwardHops = 0;

    while (true)
    {
        const int32* IndexPtr = Block.LabelIndex.Find(CurrentLabel);
        if (!IndexPtr)
        {
            return -1;
        }

        const int32 StartIdx = *IndexPtr;

        // Scan forward from the label to find the first instruction whose emitted K2Node
        // is a valid exec TARGET: one that has an exec INPUT pin able to receive control.
        // The K2Node's actual pin shape, not the authored BPIR opcode, is the source of
        // truth for "does this node sit in the exec chain", and requiring an exec INPUT pin
        // specifically handles BOTH directions of an opcode/pin-shape mismatch:
        //
        //   (1) RestoreExecIfPure can flip Inst.Opcode from Call to Pure when a CallFunction
        //       node ends up with no exec pins (compile-time discovery that the function is
        //       BlueprintPure). An impure K2Node such as K2Node_DynamicCast at a labeled
        //       position must still be selected even though its opcode now reads Pure: it has
        //       an exec input pin, so it is. (Fixes the silent miss in
        //       B-bpir-statement-cast-success-unwired-replace-shared-topology, where such a
        //       statement-form cast was incorrectly skipped.)
        //   (2) The generic-emit lane (CreateGenericK2Node) keeps opcode Call for a PURE
        //       UK2Node emitted as `call K2Node_<Type>(...)` (e.g. K2Node_ConvertAsset).
        //       Trusting the impure opcode would mis-select this pure node, whose missing
        //       exec input pin then silently drops the exec edge and trips post-wire
        //       verification. It has no exec input pin, so it is skipped and resolution
        //       advances to the first genuinely-impure instruction. (Fixes
        //       B-bpir-exec-target-leads-pure-node.)
        //
        // GetExecInputPin(i, Inst) returns the node's first exec input pin (or null);
        // NodeHasExecPins is the wrong predicate because it also matches exec-OUTPUT-only
        // entry nodes, which can never receive control.
        FString ForwardLabel;
        bool bHasForward = false;
        for (int32 i = StartIdx; i < Block.Instructions.Num(); ++i)
        {
            FBpirInstruction& Inst = Block.Instructions[i];

            // Stop scanning if we hit another label (different scope)
            if (i > StartIdx && Inst.Opcode == EBpirOpcode::Label)
            {
                break;
            }

            // A leading `exec -> @T` (reached before any impure node) makes this an empty
            // forwarding label: control entering here is diverted to @T, so resolve to @T's
            // first impure node rather than treating the label as a dead end. An impure node
            // would have returned above, so reaching the goto means none preceded it.
            if (Inst.Opcode == EBpirOpcode::ExecGoto)
            {
                if (Inst.ExecTargets.Num() > 0)
                {
                    ForwardLabel = Inst.ExecTargets[0].Label;
                    bHasForward = true;
                }
                break;
            }

            if (Inst.Opcode == EBpirOpcode::End)
            {
                return -1;
            }

            if (GetExecInputPin(i, Inst) != nullptr)
            {
                return i;
            }
        }

        if (bHasForward)
        {
            // Cycle guard (allocation-free): a valid forwarding chain visits at most
            // LabelIndex.Num() distinct labels, so more hops than that is a cycle.
            if (++ForwardHops > Block.LabelIndex.Num())
            {
                return -1;
            }
            CurrentLabel = ForwardLabel;
            continue;
        }

        return -1;
    }
}

// ----------------------------------------------------------------------------
// GetExecInputPin()
// ----------------------------------------------------------------------------

UEdGraphPin* FBpirCompiler::GetExecInputPin(int32 InstructionIndex, const FBpirInstruction& Inst, const FString& OverrideInputPinName)
{
    const FEmittedNodeInfo* EmitInfo = EmitMap.Find(InstructionIndex);
    if (!EmitInfo || !EmitInfo->Node)
    {
        return nullptr;
    }

    // (1) Caller-supplied override — used by ExecTargets and ExecGoto wiring.
    // Hard-fails on missing pin: caller logs the compile error citing the requested name.
    if (!OverrideInputPinName.IsEmpty())
    {
        UEdGraphPin* NamedPin = EmitInfo->Node->FindPin(*OverrideInputPinName, EGPD_Input);
        if (NamedPin && NamedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return NamedPin;
        }
        return nullptr;
    }

    // (2) For Return with a named exit pin (multi-exit macros), find the specific exec pin
    if (Inst.Opcode == EBpirOpcode::Return && !Inst.TypeArg.IsEmpty())
    {
        UEdGraphPin* NamedPin = EmitInfo->Node->FindPin(*Inst.TypeArg, EGPD_Input);
        if (NamedPin && NamedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return NamedPin;
        }
    }

    // (3) Default: return first exec input pin
    for (UEdGraphPin* Pin : EmitInfo->Node->Pins)
    {
        if (Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return Pin;
        }
    }

    return nullptr;
}

// ----------------------------------------------------------------------------
// FindExecOutputPin() — maps user-facing pin names to actual node pin names
// ----------------------------------------------------------------------------

UEdGraphPin* FBpirCompiler::FindExecOutputPin(int32 InstructionIndex, const FBpirInstruction& Inst, const FString& PinName)
{
    const FEmittedNodeInfo* EmitInfo = EmitMap.Find(InstructionIndex);
    if (!EmitInfo || !EmitInfo->Node)
    {
        return nullptr;
    }

    // Normalize the requested pin name to lowercase for matching
    FString LowerPinName = PinName.ToLower();

    // Opcode-specific pin name mapping
    switch (Inst.Opcode)
    {
    case EBpirOpcode::Branch:
    {
        if (LowerPinName == TEXT("then") || LowerPinName == TEXT("true"))
        {
            UEdGraphPin* ThenPin = EmitInfo->Node->FindPin(UEdGraphSchema_K2::PN_Then);
            return ThenPin;
        }
        if (LowerPinName == TEXT("else") || LowerPinName == TEXT("false"))
        {
            UEdGraphPin* ElsePin = EmitInfo->Node->FindPin(UEdGraphSchema_K2::PN_Else);
            return ElsePin;
        }
        break;
    }

    case EBpirOpcode::Foreach:
    case EBpirOpcode::ForeachBreak:
    {
        if (LowerPinName == TEXT("body") || LowerPinName == TEXT("loop body") || LowerPinName == TEXT("loopbody"))
        {
            UEdGraphPin* Pin = EmitInfo->Node->FindPin(TEXT("Loop Body"));
            if (!Pin) Pin = EmitInfo->Node->FindPin(TEXT("LoopBody"));
            return Pin;
        }
        if (LowerPinName == TEXT("completed"))
        {
            return EmitInfo->Node->FindPin(TEXT("Completed"));
        }
        break;
    }

    case EBpirOpcode::While:
    {
        if (LowerPinName == TEXT("body") || LowerPinName == TEXT("loop body") || LowerPinName == TEXT("loopbody"))
        {
            UEdGraphPin* Pin = EmitInfo->Node->FindPin(TEXT("Loop Body"));
            if (!Pin) Pin = EmitInfo->Node->FindPin(TEXT("LoopBody"));
            return Pin;
        }
        if (LowerPinName == TEXT("completed"))
        {
            return EmitInfo->Node->FindPin(TEXT("Completed"));
        }
        break;
    }

    case EBpirOpcode::Switch:
    case EBpirOpcode::SwitchInt:
    case EBpirOpcode::SwitchString:
    case EBpirOpcode::SwitchEnum:
    {
        if (LowerPinName == TEXT("default"))
        {
            return EmitInfo->Node->FindPin(TEXT("Default"));
        }
        // For SwitchString, strip surrounding quotes — parser stores "hello" but UE pin name is hello
        FString ResolvedPinName = PinName;
        if (Inst.Opcode == EBpirOpcode::SwitchString
            && ResolvedPinName.Len() >= 2
            && ResolvedPinName.StartsWith(TEXT("\""))
            && ResolvedPinName.EndsWith(TEXT("\"")))
        {
            ResolvedPinName = ResolvedPinName.Mid(1, ResolvedPinName.Len() - 2);
        }
        if (Inst.Opcode == EBpirOpcode::SwitchEnum)
        {
            return FindSwitchEnumEntryPin(Cast<UK2Node_SwitchEnum>(EmitInfo->Node), ResolvedPinName);
        }

        return EmitInfo->Node->FindPin(*ResolvedPinName);
    }

    case EBpirOpcode::Sequence:
    {
        // Sequence pins: "0" -> "then_0", "1" -> "then_1", etc.
        FString SequencePinName = FString::Printf(TEXT("then_%s"), *PinName);
        return EmitInfo->Node->FindPin(*SequencePinName);
    }

    case EBpirOpcode::Cast:
    {
        if (LowerPinName == TEXT("success"))
        {
            // First exec output that is not "Cast Failed"
            for (UEdGraphPin* Pin : EmitInfo->Node->Pins)
            {
                if (Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                    && Pin->PinName != TEXT("CastFailed"))
                {
                    return Pin;
                }
            }
        }
        if (LowerPinName == TEXT("fail") || LowerPinName == TEXT("castfailed"))
        {
            return EmitInfo->Node->FindPin(TEXT("CastFailed"));
        }
        break;
    }

    case EBpirOpcode::Latent:
    {
        if (LowerPinName == TEXT("completed") || LowerPinName == TEXT("onfinished"))
        {
            // Try common completion pin names
            UEdGraphPin* Pin = EmitInfo->Node->FindPin(TEXT("Completed"));
            if (!Pin) Pin = EmitInfo->Node->FindPin(TEXT("OnFinished"));
            if (!Pin) Pin = EmitInfo->Node->FindPin(TEXT("Then"));
            if (!Pin)
            {
                // Fallback: second exec output pin (first is the immediate "then")
                int32 ExecCount = 0;
                for (UEdGraphPin* P : EmitInfo->Node->Pins)
                {
                    if (P->Direction == EGPD_Output
                        && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        ExecCount++;
                        if (ExecCount == 2)
                        {
                            return P;
                        }
                    }
                }
            }
            return Pin;
        }
        break;
    }

    case EBpirOpcode::Macro:
    {
        // Macro pin names match directly (e.g., "Completed" for DoOnce, "A"/"B" for FlipFlop)
        UEdGraphPin* Pin = EmitInfo->Node->FindPin(*PinName);
        if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return Pin;
        }
        // Fallback: case-insensitive + space-insensitive match
        // (decompiler strips spaces: "Is Valid" -> "IsValid", "Is Not Valid" -> "IsNotValid")
        FString NormalizedReq = PinName.Replace(TEXT(" "), TEXT(""));
        for (UEdGraphPin* P : EmitInfo->Node->Pins)
        {
            if (P->Direction == EGPD_Output
                && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                if (P->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
                {
                    return P;
                }
                FString NormalizedPin = P->PinName.ToString().Replace(TEXT(" "), TEXT(""));
                if (NormalizedPin.Equals(NormalizedReq, ESearchCase::IgnoreCase))
                {
                    return P;
                }
            }
        }
        break;
    }

    case EBpirOpcode::Call:
    case EBpirOpcode::CallDispatcher:
    case EBpirOpcode::BindDispatcher:
    case EBpirOpcode::UnbindDispatcher:
    case EBpirOpcode::ClearDispatcher:
    case EBpirOpcode::FieldNotifySubscribe:
    case EBpirOpcode::FieldNotifyUnsubscribe:
    {
        // Multi-exec call nodes: match named exec output pins directly
        UEdGraphPin* Pin = EmitInfo->Node->FindPin(*PinName);
        if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return Pin;
        }
        // Case-insensitive fallback
        for (UEdGraphPin* P : EmitInfo->Node->Pins)
        {
            if (P->Direction == EGPD_Output
                && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && P->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return P;
            }
        }
        break;
    }

    default:
        break;
    }

    // Generic fallback: try exact name match on exec output pins
    UEdGraphPin* Pin = EmitInfo->Node->FindPin(*PinName);
    if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
    {
        return Pin;
    }

    // Case-insensitive fallback across all exec output pins
    for (UEdGraphPin* P : EmitInfo->Node->Pins)
    {
        if (P->Direction == EGPD_Output
            && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            && P->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
        {
            return P;
        }
    }

    return nullptr;
}

bool FBpirCompiler::EmitGenericK2NodeInstruction(
    const FString& TypeName,
    const FBpirInstruction& Inst,
    FEmittedNodeInfo& Emit,
    UEdGraphPin*& InOutExecPin,
    TFunctionRef<bool(UFunction*, UClass*)> EmitAsyncActionFactoryNode)
{
    const bool bBareAsyncAction =
        TypeName.Equals(TEXT("K2Node_AsyncAction"), ESearchCase::IgnoreCase)
        || TypeName.Equals(TEXT("UK2Node_AsyncAction"), ESearchCase::IgnoreCase);
    if (bBareAsyncAction)
    {
        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
            TEXT("Bare K2Node_AsyncAction(...) is no longer supported because pin-set factory inference is not deterministic. Migrate to K2Node_AsyncAction_<FactoryFunctionName>(...), for example K2Node_AsyncAction_DownloadImage(...).")));
        return false;
    }

    static const FString UAsyncActionPrefix = FString(TEXT("U")) + BpirSharedConstants::AsyncAction::ClassPrefix;
    if (TypeName.StartsWith(BpirSharedConstants::AsyncAction::ClassPrefix)
        || TypeName.StartsWith(UAsyncActionPrefix))
    {
        const FString FactoryName = TypeName.StartsWith(BpirSharedConstants::AsyncAction::ClassPrefix)
            ? TypeName.RightChop(BpirSharedConstants::AsyncAction::ClassPrefixLen)
            : TypeName.RightChop(UAsyncActionPrefix.Len());

        FString ResolveError;
        UFunction* AsyncFactory = FCodeNodeEmitter::ResolveAsyncActionFactoryByName(FactoryName, &ResolveError);
        if (!AsyncFactory)
        {
            // ResolveAsyncActionFactoryByName uses IsAsyncActionFactory which opts out of
            // HasDedicatedAsyncNode owner classes, so dedicated-subclass factories never
            // surface there. The sibling helper walks UBlueprintAsyncActionBase subclasses
            // that DO carry the meta to recover the factory matching the explicit-K2-class suffix.
            UFunction* DedicatedFactory = FCodeNodeEmitter::ResolveDedicatedAsyncActionFactoryByName(FactoryName);
            if (DedicatedFactory)
            {
                if (UClass* DedicatedSubclass = FCodeNodeEmitter::ResolveDedicatedAsyncActionSubclass(DedicatedFactory))
                {
                    return EmitAsyncActionFactoryNode(DedicatedFactory, DedicatedSubclass);
                }

                // Factory was found via the dedicated-meta lane, but no matching K2 subclass
                // exists. Surface a specific diagnostic rather than the generic-lane "not found"
                // message — the factory IS there, the convention-named subclass is what's missing.
                AccumulatedErrors.Add(FCompileError(Inst.SourceLine, FString::Printf(
                    TEXT("Found dedicated-async-node factory '%s' on '%s' (HasDedicatedAsyncNode meta) but could not resolve a matching UK2Node_AsyncAction_%s subclass. Ensure the dedicated K2 subclass follows the conventional name."),
                    *FactoryName, *DedicatedFactory->GetOwnerClass()->GetPathName(), *FactoryName)));
                return false;
            }

            AccumulatedErrors.Add(FCompileError(Inst.SourceLine, ResolveError));
            return false;
        }

        // Generic-lane factory: ResolveAsyncActionFactoryByName already rejected the
        // HasDedicatedAsyncNode opt-out, so the spawn class stays the base UK2Node_AsyncAction.
        return EmitAsyncActionFactoryNode(AsyncFactory, nullptr);
    }

    TArray<FString> ProvidedArgNames;
    ProvidedArgNames.Reserve(Inst.Args.Num());
    FString ClassArgValue;
    for (const FBpirArg& Arg : Inst.Args)
    {
        ProvidedArgNames.Add(Arg.PinName);
        if (Arg.PinName.Equals(TEXT("Class"), ESearchCase::IgnoreCase))
        {
            ClassArgValue = Arg.Value;
        }
    }

    UEdGraphNode* GenericNode = NodeEmitter->CreateGenericK2Node(TypeName, CurrentGraph, ProvidedArgNames, ClassArgValue);
    if (!GenericNode)
    {
        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
            FString::Printf(TEXT("Could not construct generic K2Node '%s'"), *TypeName)));
        return false;
    }

    Emit.Node = GenericNode;

    if (Inst.NodeProps.Num() > 0)
    {
        if (UK2Node* TypedNode = Cast<UK2Node>(GenericNode))
        {
            BpirShapeMetadata::ReplayGenericNodeProps(TypedNode, Inst.NodeProps, AccumulatedErrors, Inst.SourceLine);
        }
    }

    if (InOutExecPin)
    {
        for (UEdGraphPin* Pin : GenericNode->Pins)
        {
            if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                const UEdGraphSchema* Schema = InOutExecPin->GetSchema();
                if (Schema)
                {
                    if (!Schema->TryCreateConnection(InOutExecPin, Pin))
                    {
                        UE_LOG(LogBpirCompiler, Warning, TEXT("Line %d: TryCreateConnection failed wiring exec '%s' -> '%s' on generic K2Node"),
                            Inst.SourceLine, *InOutExecPin->PinName.ToString(), *Pin->PinName.ToString());
                        AccumulatedErrors.Add(FCompileError(Inst.SourceLine,
                            FString::Printf(TEXT("TryCreateConnection failed wiring exec '%s' -> '%s' on generic K2Node"),
                                *InOutExecPin->PinName.ToString(), *Pin->PinName.ToString())));
                    }
                }
                break;
            }
        }
    }

    for (UEdGraphPin* Pin : GenericNode->Pins)
    {
        if (Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            Emit.PrimaryOutputPin = Pin;
            break;
        }
    }

    if (Inst.Opcode != EBpirOpcode::Pure)
    {
        for (UEdGraphPin* Pin : GenericNode->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                Emit.ExecOutputPin = Pin;
                InOutExecPin = Pin;
                break;
            }
        }
    }

    if (UK2Node* TypedNode = Cast<UK2Node>(GenericNode))
    {
        BpirShapeMetadata::RunPostWireHooks(TypedNode);
    }

    return true;
}
