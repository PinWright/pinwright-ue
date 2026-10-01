// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TestObjectCallFunctionRpcFixture.generated.h"

// Fixture for TestObjectCallFunctionRpcCallspace.cpp: a client-side actor with a Server RPC whose
// _Implementation re-sends itself, the shape that killed the editor in
// B-python-pie-rpcs-run-locally (URaceProgressComponent::Server_SetTotalTime).
// A standalone test world has no net driver, so the engine absorbs a client's Server RPC instead
// of sending it; GetFunctionCallspace reports that verdict as Remote and CallRemoteFunction counts
// the send, which is what a client in a networked PIE world does. Under
// FEditorScriptExecutionGuard the engine answers Local before either runs (Actor.cpp,
// AActor::GetFunctionCallspace), and the _Implementation runs instead.
UCLASS(NotPlaceable, Transient, NotBlueprintable)
class APinWrightRpcCallspaceFixture : public AActor
{
    GENERATED_BODY()

public:
    // Re-send cap: a regression ends the recursion here and fails the test instead of
    // overflowing the stack.
    static constexpr int32 MaxLocalRuns = 16;

    int32 LocalRuns = 0;
    int32 RemoteSends = 0;

    // CallInEditor lets AActor::ProcessEvent run it in a world whose actors were never
    // initialized for play, without the editor-script guard under test.
    UFUNCTION(Server, Reliable, meta = (CallInEditor = "true"))
    void Server_Resend();

    virtual int32 GetFunctionCallspace(UFunction* Function, FFrame* Stack) override;
    virtual bool CallRemoteFunction(UFunction* Function, void* Parameters, FOutParmRec* OutParms, FFrame* Stack) override;
};
