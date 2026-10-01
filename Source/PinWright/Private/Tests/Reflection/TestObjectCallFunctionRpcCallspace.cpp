// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Tests/Reflection/TestObjectCallFunctionRpcFixture.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

void APinWrightRpcCallspaceFixture::Server_Resend_Implementation()
{
    // Mirrors URaceProgressComponent: the "am I a client" check stays true when the RPC runs
    // locally on the client, so the implementation sends the RPC again.
    if (++LocalRuns < MaxLocalRuns)
    {
        Server_Resend();
    }
}

int32 APinWrightRpcCallspaceFixture::GetFunctionCallspace(UFunction* Function, FFrame* Stack)
{
    const int32 Callspace = Super::GetFunctionCallspace(Function, Stack);
    return (Callspace == FunctionCallspace::Absorbed && Function->HasAnyFunctionFlags(FUNC_NetServer))
        ? FunctionCallspace::Remote
        : Callspace;
}

bool APinWrightRpcCallspaceFixture::CallRemoteFunction(UFunction*, void*, FOutParmRec*, FFrame*)
{
    ++RemoteSends;
    return true;
}

namespace
{
    // Calls Server_Resend through object.call_function on a client-role fixture in a game world
    // and checks the RPC was sent once and never run locally.
    bool RunServerRpcThroughCallFunction(FAutomationTestBase& Test)
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::GamePreview, /*bInformEngineOfWorld=*/false);
        FScopedTransientWorldGuard WorldGuard(World);
        if (!Test.TestNotNull(TEXT("game world fixture created"), World))
        {
            return false;
        }
        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        APinWrightRpcCallspaceFixture* Actor = World->SpawnActor<APinWrightRpcCallspaceFixture>(SpawnParams);
        if (!Test.TestNotNull(TEXT("fixture actor spawned"), Actor))
        {
            return false;
        }
        Actor->SetRole(ROLE_AutonomousProxy);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Actor->GetPathName());
        Payload->SetStringField(TEXT("function"), TEXT("Server_Resend"));
        FTestResponseCapture Capture;
        if (!Test.TestTrue(TEXT("object.call_function registered"),
                InvokeHandlerWithCapture(TEXT("object.call_function"), Payload, Capture)))
        {
            return false;
        }
        Test.TestTrue(FString::Printf(TEXT("call succeeded (%s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess);
        Test.TestEqual(TEXT("the Server RPC was sent exactly once"), Actor->RemoteSends, 1);
        Test.TestEqual(TEXT("the Server RPC's _Implementation never ran on the client"), Actor->LocalRuns, 0);
        return true;
    }
}

// Counterfactual: with FEditorScriptExecutionGuard taken unconditionally (the old handler), the
// callspace is Local, the _Implementation runs and re-sends until MaxLocalRuns: LocalRuns == 16,
// RemoteSends == 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionServerRpcSentFromGameWorldTest,
    "PinWright.object.call_function.ServerRpcFromClientIsSent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionServerRpcSentFromGameWorldTest::RunTest(const FString& Parameters)
{
    return RunServerRpcThroughCallFunction(*this);
}

// Counterfactual: a handler that merely skips its own guard still inherits a caller's
// (python.execute wraps every UFUNCTION it invokes in one), and the RPC recurses locally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionServerRpcSentUnderOuterGuardTest,
    "PinWright.object.call_function.ServerRpcUnderOuterScriptGuardIsSent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionServerRpcSentUnderOuterGuardTest::RunTest(const FString& Parameters)
{
    FEditorScriptExecutionGuard OuterGuard;
    return RunServerRpcThroughCallFunction(*this);
}
