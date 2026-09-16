// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRCliffHandlers.h"
#include "AGIR/AGIRCompilerHelpers.h"


#include "AnimGraphNode_LinkedInputPose.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "IrCore/IrTextUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
using AGIRCliff::Helpers::ApplyNodeGuidIfPresent;

// Strip optional surrounding quote/name wrappers so reflective ImportText calls
// receive the bare payload. Mirrors UnquoteIfQuoted in AGIRCompiler.cpp; kept
// local to avoid coupling this TU to that anonymous namespace helper.
FString UnquoteForImport(const FString& Value)
{
    FString Unwrapped;
    FString Error;
    if (FIrTextUtils::TryUnwrapStringLiteral(Value, Unwrapped, Error)
        || FIrTextUtils::TryUnwrapNameToken(Value, Unwrapped, Error))
    {
        return Unwrapped;
    }
    return Value;
}

// Reflective write onto a class-level UPROPERTY of the editor UObject (Inputs,
// FunctionReference, InputPoseIndex live on UAnimGraphNode_LinkedInputPose
// itself, not on the runtime FAnimNode_LinkedInputPose struct).
bool TryWriteEditorClassField(UAnimGraphNode_LinkedInputPose* Node, FName FieldName, const FString& ValueAsText, FString& OutError)
{
    if (!Node)
    {
        OutError = TEXT("Node is null");
        return false;
    }
    FProperty* Property = Node->GetClass()->FindPropertyByName(FieldName);
    if (!Property)
    {
        OutError = FString::Printf(TEXT("Field '%s' not found on %s"),
            *FieldName.ToString(), *Node->GetClass()->GetName());
        return false;
    }
    void* FieldData = Property->ContainerPtrToValuePtr<void>(Node);
    const FString Unquoted = UnquoteForImport(ValueAsText);
    const TCHAR* Result = Property->ImportText_Direct(*Unquoted, FieldData, /*OwnerObject=*/Node, PPF_None);
    if (!Result)
    {
        OutError = FString::Printf(TEXT("ImportText failed for '%s' on %s"),
            *FieldName.ToString(), *Node->GetClass()->GetName());
        return false;
    }
    return true;
}

} // namespace

bool AGIRCliff::CompileLinkedInputPoseInstruction(
    UAnimBlueprint* /*AnimBP*/,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& /*PendingWires*/,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult)
{
    if (!TargetGraph)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            FString::Printf(TEXT("linked_input_pose has no target graph (line %d)."), Inst.SourceLine));
        return false;
    }

    // Determine the runtime input-pose name from `Name=` arg if supplied; the
    // lead SymbolName carries the editor object's UE name (e.g. "LinkedInputPose_0")
    // which the upsert fallback uses when `Name=` is omitted.
    FName ParamName;
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name == TEXT("Name"))
        {
            const FString Unquoted = UnquoteForImport(Arg.Value);
            ParamName = FName(*Unquoted);
            break;
        }
    }
    const FName FallbackName = !Inst.SymbolName.IsEmpty() ? FName(*Inst.SymbolName) : NAME_None;

    // Upsert: layer-implementation graphs (and CreateFunctionGraphTerminators)
    // pre-populate a UAnimGraphNode_LinkedInputPose per pose parameter. Naive
    // create-only would yield duplicates after Replace+compile when the schema
    // re-seeds during graph creation.
    UAnimGraphNode_LinkedInputPose* EditorNode = nullptr;
    for (UEdGraphNode* GraphNode : TargetGraph->Nodes)
    {
        UAnimGraphNode_LinkedInputPose* Existing = Cast<UAnimGraphNode_LinkedInputPose>(GraphNode);
        if (!Existing)
        {
            continue;
        }
        const FName ExistingName = Existing->Node.Name;
        if (!ParamName.IsNone() && ExistingName == ParamName)
        {
            EditorNode = Existing;
            break;
        }
        if (ParamName.IsNone() && !FallbackName.IsNone() && Existing->GetFName() == FallbackName)
        {
            EditorNode = Existing;
            break;
        }
    }

    if (!EditorNode)
    {
        const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;
        UAnimGraphNode_Base* NewNode = AnimGraphConstructionUtils::CreateAnimNode(
            TargetGraph, UAnimGraphNode_LinkedInputPose::StaticClass(), Position);
        EditorNode = Cast<UAnimGraphNode_LinkedInputPose>(NewNode);
        if (!EditorNode)
        {
            OutResult = FAGIRCompileResult::MakeError(
                TEXT("AGIR_NODE_CREATE_FAILED"),
                FString::Printf(TEXT("Failed to create UAnimGraphNode_LinkedInputPose on graph '%s' (line %d)."),
                    *TargetGraph->GetName(), Inst.SourceLine));
            return false;
        }
    }

    ApplyNodeGuidIfPresent(EditorNode, Inst.NodeGuid);

    // Apply args. `Name` (and any other runtime FAnimNode_LinkedInputPose
    // field) flows through WriteAnimNodeArg onto the runtime struct;
    // `Inputs`, `FunctionReference`, `InputPoseIndex` live as class-level
    // UPROPERTY on the editor UObject and round-trip via ImportText_Direct on
    // the editor class properties.
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty())
        {
            continue;
        }
        const FName FieldName(*Arg.Name);
        const FString WriteError = AGIRCliff::Helpers::WriteAnimNodeArg(
            EditorNode, FieldName, Arg.Value);
        if (WriteError.IsEmpty())
        {
            continue;
        }
        // Runtime struct lookup missed; fall back to the editor class.
        FString ClassError;
        if (TryWriteEditorClassField(EditorNode, FieldName, Arg.Value, ClassError))
        {
            continue;
        }
        OutWarnings.Add(FString::Printf(
            TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
            *ClassError, Inst.SourceLine));
    }

    // Refresh editor pins so any FunctionReference / Inputs change materialises
    // the matching pose-parameter pins; mirrors the linked_anim handler's
    // ReconstructNode call after layer-id resolution.
    EditorNode->ReconstructNode();

    if (!Inst.ResultName.IsEmpty())
    {
        Symbols.Add(Inst.ResultName, EditorNode);
    }

    ++OutNodesCreated;
    return true;
}
