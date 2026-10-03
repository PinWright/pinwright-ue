// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/Object.h"

#include "DispatcherRecorder.generated.h"

// The object blueprint.record_dispatcher binds into a dynamic multicast delegate's invocation
// list. A dynamic delegate can only call a UFUNCTION by name, and the dispatcher's signature is
// arbitrary, so the bound function takes no parameters and ProcessEvent is overridden instead:
// TScriptDelegate::ProcessDelegate hands ProcessEvent the broadcaster's parameter buffer, which
// is laid out for the delegate's SignatureFunction, and OnFired reads it from there. The no-param
// function body never runs.
UCLASS(Transient)
class UPinWrightDispatcherRecorder : public UObject
{
    GENERATED_BODY()

public:
    static FName BoundFunctionName();

    // Called on every broadcast with the signature-shaped parameter buffer (may be null for a
    // parameterless signature).
    TFunction<void(void* /*Parms*/)> OnFired;

    UFUNCTION()
    void OnDispatcherFired() {}

    virtual void ProcessEvent(UFunction* Function, void* Parms) override;
};
