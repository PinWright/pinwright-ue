// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIRPinResolver.h
//
// Compile-side helper. Translates an FCRIRArg whose `bIsLocalRef=true` into a
// URigVMController::AddLink call. Wire-direction convention mirrors AGIR:
//   wire_in_<PinName>=%src.srcPin   -> AddLink(srcPath, target.PinName)
//   wire_out_<PinName>=%dst.dstPin  -> AddLink(target.PinName, dstPath)

#pragma once

#include "CoreMinimal.h"
#include "CRIR/CRIROpcodes.h"


class URigVMController;
class URigVMNode;

class FCRIRPinResolver
{
public:
    // Resolves `Arg` against `LocalIdSymbolMap` and applies the link via
    // `Controller`. `ArgName` is the full arg name (e.g. `wire_in_Pose`) and
    // identifies both the wiring direction and the pin name on `TargetNode`.
    // Returns false on any failure with a descriptive message in `OutError`.
    static bool ResolveAndConnect(
        URigVMController* Controller,
        URigVMNode* TargetNode,
        const FString& ArgName,
        const FCRIRArg& Arg,
        const TMap<FString, URigVMNode*>& LocalIdSymbolMap,
        FString& OutError);

    // Builds `<NodePath>.<PinPath>` per RigVM pin-path convention.
    static FString MakePinPath(URigVMNode* Node, const FString& PinPath);
};
