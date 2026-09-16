// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRPinResolver.h"


#include "CRIR/CRIRWireNaming.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMNode.h"

using CRIRWireNaming::WireInPrefix;
using CRIRWireNaming::WireOutPrefix;

FString FCRIRPinResolver::MakePinPath(URigVMNode* Node, const FString& PinPath)
{
    if (!Node)
    {
        return PinPath;
    }
    if (PinPath.IsEmpty())
    {
        return Node->GetNodePath();
    }
    return FString::Printf(TEXT("%s.%s"), *Node->GetNodePath(), *PinPath);
}

bool FCRIRPinResolver::ResolveAndConnect(
    URigVMController* Controller,
    URigVMNode* TargetNode,
    const FString& ArgName,
    const FCRIRArg& Arg,
    const TMap<FString, URigVMNode*>& LocalIdSymbolMap,
    FString& OutError)
{
    if (!Controller || !TargetNode)
    {
        OutError = TEXT("CRIR_INVALID_TARGET");
        return false;
    }
    if (!Arg.bIsLocalRef)
    {
        OutError = TEXT("CRIR_NOT_LOCAL_REF");
        return false;
    }

    bool bWireIn = false;
    FString TargetPinName;
    if (ArgName.StartsWith(WireInPrefix))
    {
        bWireIn = true;
        TargetPinName = ArgName.Mid(FCString::Strlen(WireInPrefix));
    }
    else if (ArgName.StartsWith(WireOutPrefix))
    {
        bWireIn = false;
        TargetPinName = ArgName.Mid(FCString::Strlen(WireOutPrefix));
    }
    else
    {
        OutError = FString::Printf(TEXT("CRIR_UNKNOWN_WIRE_DIRECTION:%s"), *ArgName);
        return false;
    }

    if (TargetPinName.IsEmpty())
    {
        OutError = FString::Printf(TEXT("CRIR_EMPTY_PIN_NAME:%s"), *ArgName);
        return false;
    }

    URigVMNode* const* SourceNodePtr = LocalIdSymbolMap.Find(Arg.LocalRefNode);
    if (!SourceNodePtr || !*SourceNodePtr)
    {
        OutError = FString::Printf(TEXT("CRIR_SYMBOL_NOT_FOUND:%s"), *Arg.LocalRefNode);
        return false;
    }
    URigVMNode* SourceNode = *SourceNodePtr;

    const FString SourcePath = MakePinPath(SourceNode, Arg.LocalRefPin);
    const FString TargetPath = MakePinPath(TargetNode, TargetPinName);

    // wire_in: data flows source -> target (source is output side).
    // wire_out: data flows target -> source (target is output side).
    const FString OutputPinPath = bWireIn ? SourcePath : TargetPath;
    const FString InputPinPath  = bWireIn ? TargetPath : SourcePath;

    // Compiler owns the FScopedTransaction at a higher level — disable per-call
    // undo to avoid nested transactions and Python noise on every link.
    if (!Controller->AddLink(OutputPinPath, InputPinPath, /*bSetupUndoRedo*/ false, /*bPrintPythonCommand*/ false))
    {
        OutError = FString::Printf(TEXT("CRIR_LINK_FAILED:%s->%s"), *OutputPinPath, *InputPinPath);
        return false;
    }

    OutError.Reset();
    return true;
}
