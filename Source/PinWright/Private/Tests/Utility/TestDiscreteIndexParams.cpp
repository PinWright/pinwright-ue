// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-discrete-index-params-truncate-fractions.
// The direct handler harness intentionally bypasses FRpcDispatcher's declared-type gate, so
// these calls prove the shared RequireInt path rejects bad values before an array mutation or
// session lookup can use a truncated index.
#include "TestContainerValueTypeHost.h"
#include "Misc/AutomationTest.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

namespace
{
TSharedPtr<FJsonObject> MakeArrayPayload(const FString& ObjectPath, double Index, bool bSetValue)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), ObjectPath);
    Payload->SetStringField(TEXT("propertyName"), TEXT("PoseList"));
    Payload->SetNumberField(TEXT("index"), Index);
    if (bSetValue)
    {
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetStringField(TEXT("Label"), TEXT("must-not-land"));
        Value->SetNumberField(TEXT("Weight"), 999);
        Payload->SetObjectField(TEXT("value"), Value);
    }
    return Payload;
}

void AssertArrayIndexRejected(FAutomationTestBase& Test, const TCHAR* Method,
    const FString& ObjectPath, double Index)
{
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        Method, MakeArrayPayload(ObjectPath, Index, FCString::Strcmp(Method, TEXT("container.array.set")) == 0),
        Capture);
    Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), Method), bFound);
    Test.TestFalse(*FString::Printf(TEXT("%s rejects index %.3f"), Method, Index), Capture.bSuccess);
    Test.TestEqual(*FString::Printf(TEXT("%s reports INVALID_PARAMS"), Method),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    Test.TestTrue(*FString::Printf(TEXT("%s error names index"), Method),
        Capture.Message.Contains(TEXT("index")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiscreteIndexParamsValidationTest,
    "PinWright.container.DiscreteIndexParams.RejectsFractionalAndOverflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDiscreteIndexParamsValidationTest::RunTest(const FString& Parameters)
{
    UTestContainerValueTypeHost* Host = NewObject<UTestContainerValueTypeHost>(
        GetTransientPackage(), TEXT("FDiscreteIndexParamsValidationTest_Host"));
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host)
    {
        return false;
    }

    Host->PoseList.AddDefaulted(3);
    Host->PoseList[0].Label = TEXT("zero");
    Host->PoseList[0].Weight = 10;
    Host->PoseList[1].Label = TEXT("one");
    Host->PoseList[1].Weight = 20;
    Host->PoseList[2].Label = TEXT("two");
    Host->PoseList[2].Weight = 30;

    const FString ObjectPath = Host->GetPathName();
    TestTrue(TEXT("Host path resolves to same object"),
        FindObject<UObject>(nullptr, *ObjectPath) == Host);

    const TArray<const TCHAR*> ArrayMethods = {
        TEXT("container.array.remove"),
        TEXT("container.array.set")
    };
    for (const TCHAR* Method : ArrayMethods)
    {
        AssertArrayIndexRejected(*this, Method, ObjectPath, 1.5);
        AssertArrayIndexRejected(*this, Method, ObjectPath, 2147483648.0);
        AssertArrayIndexRejected(*this, Method, ObjectPath, -2147483649.0);
    }

    FTestResponseCapture ValidCapture;
    const bool bValidGetFound = InvokeHandlerWithCapture(
        TEXT("container.array.get"), MakeArrayPayload(ObjectPath, 1.0, false), ValidCapture);
    TestTrue(TEXT("container.array.get handler registered for integral index"), bValidGetFound);
    TestTrue(TEXT("container.array.get accepts valid integral index"), ValidCapture.bSuccess);

    TestEqual(TEXT("invalid array indices preserve element count"), Host->PoseList.Num(), 3);
    TestEqual(TEXT("invalid array indices preserve element zero"), Host->PoseList[0].Label, FString(TEXT("zero")));
    TestEqual(TEXT("invalid array indices preserve element one"), Host->PoseList[1].Label, FString(TEXT("one")));
    TestEqual(TEXT("invalid array indices preserve element two"), Host->PoseList[2].Label, FString(TEXT("two")));

    for (double PlayerIndex : { 1.5, 2147483648.0, -2147483649.0 })
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("playerIndex"), PlayerIndex);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("session.remove_local_player"), Payload, Capture);
        TestTrue(TEXT("session.remove_local_player handler registered"), bFound);
        TestFalse(TEXT("session.remove_local_player rejects invalid index"), Capture.bSuccess);
        TestEqual(TEXT("session.remove_local_player reports INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("session.remove_local_player error names playerIndex"),
            Capture.Message.Contains(TEXT("playerIndex")));
    }

    return true;
}
