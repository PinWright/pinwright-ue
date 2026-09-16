// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UClass;
class UK2Node;
struct FCompileError;

namespace BpirShapeMetadata
{
    struct FBpirShapeDescriptor
    {
        TSet<FName> PreAllocateProperties;
        TFunction<bool(UK2Node* Node, const TMap<FString, FString>& NodeProps, FString& OutError)> PreAllocateHook;
        // Keys whose application is owned by PreAllocateHook (e.g. pin values, not UPROPERTYs).
        // The reflective apply loop must skip these so it does not error on missing properties.
        TSet<FName> HookHandledProperties;
    };

    const FBpirShapeDescriptor* FindBpirShapeDescriptor(UClass* NodeClass);

    bool ReplayGenericNodeProps(UK2Node* Node, const TMap<FString, FString>& NodeProps, TArray<FCompileError>& OutErrors, int32 SourceLine);

    void RunPostWireHooks(UK2Node* Node);
}
