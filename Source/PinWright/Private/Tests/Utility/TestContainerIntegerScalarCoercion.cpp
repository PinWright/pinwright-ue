// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-container-integer-key-element-coerces-zero.
//
// Integer map keys and set elements must be parsed by the shared strict scalar
// converter before a handler modifies or scans a reflected container. The old
// handlers used Atoi/casts that turned malformed text and fractional values into
// valid integer zero (or another truncated value), allowing a request to target
// an unrelated existing entry while returning success.
#include "TestContainerIntegerScalarCoercionHost.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

namespace
{
TSharedPtr<FJsonObject> MakeMapSetPayload(const FString& ObjectPath,
    const TSharedPtr<FJsonValue>& Key, int32 Value)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), ObjectPath);
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntKeyMap"));
    Payload->SetField(TEXT("key"), Key);
    Payload->SetNumberField(TEXT("value"), Value);
    return Payload;
}

TSharedPtr<FJsonObject> MakeMapKeyPayload(const FString& ObjectPath,
    const TSharedPtr<FJsonValue>& Key)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), ObjectPath);
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntKeyMap"));
    Payload->SetField(TEXT("key"), Key);
    return Payload;
}

TSharedPtr<FJsonObject> MakeSetPayload(const FString& ObjectPath,
    const TSharedPtr<FJsonValue>& Value)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), ObjectPath);
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntSet"));
    Payload->SetField(TEXT("value"), Value);
    return Payload;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerIntegerScalarCoercionTest,
    "PinWright.container.IntegerScalarCoercion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerIntegerScalarCoercionTest::RunTest(const FString& Parameters)
{
    UTestContainerIntegerScalarCoercionHost* Host = NewObject<UTestContainerIntegerScalarCoercionHost>(
        GetTransientPackage(), TEXT("FContainerIntegerScalarCoercionTest_Host"));
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host)
    {
        return false;
    }

    Host->IntKeyMap.Add(0, 100);
    Host->IntKeyMap.Add(7, 700);
    Host->IntSet.Add(0);
    Host->IntSet.Add(7);

    const FString ObjectPath = Host->GetPathName();
    TestTrue(TEXT("Host path resolves to same object"),
        FindObject<UObject>(nullptr, *ObjectPath) == Host);

    auto AssertMapUnchanged = [this, Host](const TCHAR* Label)
    {
        TestEqual(*FString::Printf(TEXT("%s keeps map size"), Label), Host->IntKeyMap.Num(), 2);
        const int32* ZeroValue = Host->IntKeyMap.Find(0);
        TestNotNull(*FString::Printf(TEXT("%s keeps zero key"), Label), ZeroValue);
        if (ZeroValue)
        {
            TestEqual(*FString::Printf(TEXT("%s keeps zero value"), Label), *ZeroValue, 100);
        }
        const int32* SevenValue = Host->IntKeyMap.Find(7);
        TestNotNull(*FString::Printf(TEXT("%s keeps nonzero key"), Label), SevenValue);
        if (SevenValue)
        {
            TestEqual(*FString::Printf(TEXT("%s keeps nonzero value"), Label), *SevenValue, 700);
        }
    };

    auto AssertMapSetRejected = [this, &ObjectPath, &AssertMapUnchanged](
        const TCHAR* Label, const TSharedPtr<FJsonValue>& Key)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.map.set"), MakeMapSetPayload(ObjectPath, Key, 999), Capture);
        TestTrue(*FString::Printf(TEXT("%s handler registered"), Label), bFound);
        TestFalse(*FString::Printf(TEXT("%s refused"), Label), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s reports INVALID_PARAMS"), Label),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        AssertMapUnchanged(Label);
    };

    AssertMapSetRejected(TEXT("malformed map key"),
        MakeShared<FJsonValueString>(TEXT("not-an-int")));
    AssertMapSetRejected(TEXT("fractional map key"),
        MakeShared<FJsonValueString>(TEXT("1.5")));
    AssertMapSetRejected(TEXT("overflowing map key"),
        MakeShared<FJsonValueString>(TEXT("2147483648")));
    AssertMapSetRejected(TEXT("non-scalar map key"),
        MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));

    // Every map key lookup path must reject the same malformed key before scanning.
    for (const TCHAR* Method : { TEXT("container.map.get"), TEXT("container.map.remove"),
        TEXT("container.map.has_key") })
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            Method, MakeMapKeyPayload(ObjectPath, MakeShared<FJsonValueString>(TEXT("not-an-int"))),
            Capture);
        TestTrue(*FString::Printf(TEXT("%s handler registered"), Method), bFound);
        TestFalse(*FString::Printf(TEXT("%s refused malformed key"), Method), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s reports INVALID_PARAMS"), Method),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        AssertMapUnchanged(Method);
    }

    // A valid non-canonical spelling is converted once and echoed canonically.
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.map.set"),
            MakeMapSetPayload(ObjectPath, MakeShared<FJsonValueString>(TEXT("00012")), 1200),
            Capture);
        TestTrue(TEXT("canonical map key handler registered"), bFound);
        TestTrue(TEXT("canonical map key succeeded"), Capture.bSuccess);
        FString ResponseKey;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("key"), ResponseKey);
        }
        TestEqual(TEXT("map response uses canonical integer key"), ResponseKey, FString(TEXT("12")));
        const int32* StoredValue = Host->IntKeyMap.Find(12);
        TestNotNull(TEXT("canonical map key stored"), StoredValue);
        if (StoredValue)
        {
            TestEqual(TEXT("canonical map value stored"), *StoredValue, 1200);
        }
    }

    auto AssertSetUnchanged = [this, Host](const TCHAR* Label)
    {
        TestEqual(*FString::Printf(TEXT("%s keeps set size"), Label), Host->IntSet.Num(), 2);
        TestTrue(*FString::Printf(TEXT("%s keeps zero element"), Label), Host->IntSet.Contains(0));
        TestTrue(*FString::Printf(TEXT("%s keeps nonzero element"), Label), Host->IntSet.Contains(7));
    };

    auto AssertSetRejected = [this, &ObjectPath, &AssertSetUnchanged](
        const TCHAR* Method, const TCHAR* Label, const TSharedPtr<FJsonValue>& Value)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            Method, MakeSetPayload(ObjectPath, Value), Capture);
        TestTrue(*FString::Printf(TEXT("%s handler registered"), Method), bFound);
        TestFalse(*FString::Printf(TEXT("%s refused %s"), Method, Label), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s reports INVALID_PARAMS for %s"), Method, Label),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        AssertSetUnchanged(Label);
    };

    const TArray<const TCHAR*> SetMethods = {
        TEXT("container.set.add"), TEXT("container.set.remove"), TEXT("container.set.contains") };
    for (const TCHAR* Method : SetMethods)
    {
        AssertSetRejected(Method, TEXT("malformed element"),
            MakeShared<FJsonValueString>(TEXT("not-an-int")));
    }
    AssertSetRejected(TEXT("container.set.add"), TEXT("fractional element"),
        MakeShared<FJsonValueNumber>(1.5));
    AssertSetRejected(TEXT("container.set.remove"), TEXT("overflowing element"),
        MakeShared<FJsonValueString>(TEXT("2147483648")));
    AssertSetRejected(TEXT("container.set.contains"), TEXT("non-scalar element"),
        MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));

    // A native whole JSON number remains a valid set element.
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("container.set.add"), MakeSetPayload(ObjectPath, MakeShared<FJsonValueNumber>(9.0)), Capture);
        TestTrue(TEXT("valid integer set handler registered"), bFound);
        TestTrue(TEXT("valid integer set element succeeded"), Capture.bSuccess);
        TestTrue(TEXT("valid integer set element stored"), Host->IntSet.Contains(9));
    }

    return true;
}
