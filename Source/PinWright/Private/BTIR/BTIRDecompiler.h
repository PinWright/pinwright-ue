// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UBehaviorTree;
class UBlackboardData;


struct FBTIRResult
{
    bool bSuccess = false;
    FString Text;
    TArray<FString> Warnings;

    static FBTIRResult MakeError(const FString& Message)
    {
        FBTIRResult Result;
        Result.Warnings.Add(Message);
        return Result;
    }
};

class PINWRIGHT_API BTIRDecompiler
{
public:
    static FBTIRResult BuildBehaviorTreeIrText(UBehaviorTree* BehaviorTree);
    static FBTIRResult BuildBlackboardIrText(UBlackboardData* Blackboard);

    // Drops the per-class blackboard-selector property cache. Mandatory before any
    // garbage collection that may free Blueprint-generated node classes: the cache
    // holds raw FStructProperty* that the collect invalidates silently.
    static void ClearPropertyCaches();
};
