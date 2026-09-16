// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Utils/PropertyImport.h"

#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include "TestPropertyImportMalformedScalarsHost.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyImportMalformedScalarsTest,
    "PinWright.utils.property_import.MalformedScalarsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyImportMalformedScalarsTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient scalar host created"), Host);
    if (!Host)
    {
        return false;
    }

    FProperty* BoolProp = Host->GetClass()->FindPropertyByName(TEXT("BoolValue"));
    FProperty* FloatProp = Host->GetClass()->FindPropertyByName(TEXT("FloatValue"));
    FProperty* FixedFloatProp = Host->GetClass()->FindPropertyByName(TEXT("FixedFloatValues"));
    FProperty* DoubleProp = Host->GetClass()->FindPropertyByName(TEXT("DoubleValue"));
    FProperty* IntProp = Host->GetClass()->FindPropertyByName(TEXT("IntValue"));
    FProperty* Int64Prop = Host->GetClass()->FindPropertyByName(TEXT("Int64Value"));
    FProperty* UInt16Prop = Host->GetClass()->FindPropertyByName(TEXT("UInt16Value"));
    FProperty* FixedIntProp = Host->GetClass()->FindPropertyByName(TEXT("FixedIntValues"));
    TestNotNull(TEXT("BoolValue property found"), BoolProp);
    TestNotNull(TEXT("FloatValue property found"), FloatProp);
    TestNotNull(TEXT("FixedFloatValues property found"), FixedFloatProp);
    TestNotNull(TEXT("DoubleValue property found"), DoubleProp);
    TestNotNull(TEXT("IntValue property found"), IntProp);
    TestNotNull(TEXT("Int64Value property found"), Int64Prop);
    TestNotNull(TEXT("UInt16Value property found"), UInt16Prop);
    TestNotNull(TEXT("FixedIntValues property found"), FixedIntProp);
    if (!BoolProp || !FloatProp || !FixedFloatProp || !DoubleProp || !IntProp
        || !Int64Prop || !UInt16Prop || !FixedIntProp)
    {
        return false;
    }

    FString Error;
    bool bApplied = ApplyJsonValueToProperty(
        Host, BoolProp, MakeShared<FJsonValueString>(TEXT("enabled")), Error);
    TestFalse(FString::Printf(TEXT("Malformed bool refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Malformed bool reports an error"), !Error.IsEmpty());
    TestTrue(TEXT("Malformed bool leaves the existing value unchanged"), Host->BoolValue);

    Error.Reset();
    bApplied = ApplyJsonValueToProperty(
        Host, FloatProp, MakeShared<FJsonValueString>(TEXT("2s")), Error);
    TestFalse(FString::Printf(TEXT("Trailing numeric suffix refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Malformed float reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("Malformed float leaves the existing value unchanged"), Host->FloatValue, 1.25f);

    Error.Reset();
    bApplied = ApplyJsonValueToProperty(
        Host, IntProp, MakeShared<FJsonValueString>(TEXT("12.5")), Error);
    TestFalse(FString::Printf(TEXT("Fractional integer refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Fractional integer reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("Fractional integer leaves the existing value unchanged"), Host->IntValue, 123);

    Error.Reset();
    bApplied = ApplyJsonValueToProperty(
        Host, Int64Prop, MakeShared<FJsonValueString>(TEXT("9223372036854775808")), Error);
    TestFalse(FString::Printf(TEXT("Overflowing integer refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Overflowing integer reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("Overflow leaves the existing int64 unchanged"), Host->Int64Value, static_cast<int64>(456));

    Error.Reset();
    bApplied = ApplyJsonValueToProperty(
        Host, UInt16Prop, MakeShared<FJsonValueString>(TEXT("-1")), Error);
    TestFalse(FString::Printf(TEXT("Negative unsigned integer refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Negative unsigned integer reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("Negative unsigned integer leaves the existing value unchanged"), Host->UInt16Value, static_cast<uint16>(7));

    Error.Reset();
    bApplied = ApplyJsonValueToProperty(
        Host, DoubleProp,
        MakeShared<FJsonValueNumber>(std::numeric_limits<double>::infinity()), Error);
    TestFalse(FString::Printf(TEXT("Non-finite number refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Non-finite number reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("Non-finite number leaves the existing double unchanged"), Host->DoubleValue, 2.5);

    Error.Reset();
    bApplied = ApplyJsonValueToProperty(
        Host, FixedFloatProp,
        MakeShared<FJsonValueNumber>(static_cast<double>(std::numeric_limits<float>::max()) * 2.0),
        Error);
    TestFalse(FString::Printf(TEXT("Fixed-array float overflow refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Fixed-array float overflow reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("Fixed-array float first element remains unchanged"),
        Host->FixedFloatValues[0], 1.25f);
    TestEqual(TEXT("Fixed-array float sibling remains unchanged"),
        Host->FixedFloatValues[1], 2.5f);

    TSharedPtr<FJsonValue> Coerced = CoerceStringToJsonValueByProperty(
        TEXT("not-a-number"), IntProp);
    TestNotNull(TEXT("Malformed scalar coercion returned a value"), Coerced.Get());
    if (Coerced.IsValid())
    {
        TestTrue(TEXT("Malformed scalar coercion preserves the source string"),
                 Coerced->Type == EJson::String);
    }

    Error.Reset();
    bApplied = ApplyJsonValueToProperty(Host, IntProp, Coerced, Error);
    TestFalse(FString::Printf(TEXT("Coerced malformed integer refused: %s"), *Error), bApplied);
    TestTrue(TEXT("Coerced malformed integer reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("Coerced malformed integer leaves the existing value unchanged"), Host->IntValue, 123);

    Error.Reset();
    bool bImported = ImportTextToProperty(
        Host, IntProp, TEXT("321trailing"), Error);
    TestFalse(FString::Printf(TEXT("ImportText trailing input refused: %s"), *Error), bImported);
    TestTrue(TEXT("ImportText trailing input reports an error"), !Error.IsEmpty());
    TestEqual(TEXT("ImportText trailing input leaves the existing value unchanged"), Host->IntValue, 123);

    Error.Reset();
    bImported = ImportTextToProperty(Host, FixedIntProp, TEXT("321"), Error);
    TestTrue(FString::Printf(TEXT("ImportText fixed-array value accepted: %s"), *Error), bImported);
    TestTrue(TEXT("ImportText fixed-array value has no error"), Error.IsEmpty());
    TestEqual(TEXT("ImportText fixed-array updates the first element"),
        Host->FixedIntValues[0], 321);
    TestEqual(TEXT("ImportText fixed-array preserves the sibling element"),
        Host->FixedIntValues[1], 22);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetMalformedScalarNoMutationTest,
    "PinWright.property.set.MalformedScalarRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetMalformedScalarNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient scalar handler host created"), Host);
    if (!Host)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntValue"));
    Payload->SetStringField(TEXT("value"), TEXT("not-a-number"));

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("property.set handler registered"), bFound);
    TestFalse(TEXT("property.set malformed scalar is rejected"), Capture.bSuccess);
    TestEqual(TEXT("property.set reports PROPERTY_CONVERSION_FAILED"),
        Capture.ErrorCode, FString(TEXT("PROPERTY_CONVERSION_FAILED")));
    TestTrue(TEXT("property.set error message is non-empty"), !Capture.Message.IsEmpty());
    TestEqual(TEXT("property.set leaves scalar unchanged"), Host->IntValue, 123);
    TestEqual(TEXT("property.set malformed scalar does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetFixedArrayScalarIsolationTest,
    "PinWright.property.set.FixedArrayScalarIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetFixedArrayScalarIsolationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient fixed-array handler host created"), Host);
    if (!Host)
    {
        return false;
    }

    FProperty* FixedArrayProp = Host->GetClass()->FindPropertyByName(TEXT("FixedIntValues"));
    TestNotNull(TEXT("FixedIntValues property found"), FixedArrayProp);
    if (!FixedArrayProp)
    {
        return false;
    }
    TestEqual(TEXT("FixedIntValues has two reflected elements"), FixedArrayProp->ArrayDim, 2);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("FixedIntValues"));
    Payload->SetNumberField(TEXT("value"), 31);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture);

    TestTrue(TEXT("property.set fixed-array handler registered"), bFound);
    TestTrue(TEXT("property.set fixed-array scalar succeeds"), Capture.bSuccess);
    TestEqual(TEXT("property.set updates only the targeted fixed-array element"),
        Host->FixedIntValues[0], 31);
    TestEqual(TEXT("property.set preserves the fixed-array sibling"),
        Host->FixedIntValues[1], 22);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetMalformedArrayScalarNoMutationTest,
    "PinWright.property.set.MalformedArrayScalarNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetMalformedArrayScalarNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient malformed-array handler host created"), Host);
    if (!Host)
    {
        return false;
    }
    Host->IntArray.Add(77);

    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Add(MakeShared<FJsonValueNumber>(31.0));
    Values.Add(MakeShared<FJsonValueString>(TEXT("not-a-number")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntArray"));
    Payload->SetArrayField(TEXT("value"), Values);

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("property.set malformed-array handler registered"), bFound);
    TestFalse(TEXT("property.set malformed array scalar is rejected"), Capture.bSuccess);
    TestEqual(TEXT("property.set malformed array reports PROPERTY_CONVERSION_FAILED"),
        Capture.ErrorCode, FString(TEXT("PROPERTY_CONVERSION_FAILED")));
    TestTrue(TEXT("property.set malformed array error message is non-empty"), !Capture.Message.IsEmpty());
    TestEqual(TEXT("property.set malformed array leaves the existing element unchanged"),
        Host->IntArray[0], 77);
    TestEqual(TEXT("property.set malformed array leaves the array size unchanged"),
        Host->IntArray.Num(), 1);
    TestEqual(TEXT("property.set malformed array does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArraySetMalformedScalarNoMutationTest,
    "PinWright.container.array.set.MalformedScalarRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArraySetMalformedScalarNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient array handler host created"), Host);
    if (!Host)
    {
        return false;
    }
    Host->IntArray.Add(77);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntArray"));
    Payload->SetNumberField(TEXT("index"), 0);
    Payload->SetStringField(TEXT("value"), TEXT("not-a-number"));

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("container.array.set"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("container.array.set handler registered"), bFound);
    TestFalse(TEXT("container.array.set malformed scalar is rejected"), Capture.bSuccess);
    TestEqual(TEXT("container.array.set reports UNSUPPORTED_TYPE"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
    TestTrue(TEXT("container.array.set error message is non-empty"), !Capture.Message.IsEmpty());
    TestEqual(TEXT("container.array.set leaves the element unchanged"), Host->IntArray[0], 77);
    TestEqual(TEXT("container.array.set leaves the array size unchanged"), Host->IntArray.Num(), 1);
    TestEqual(TEXT("container.array.set malformed scalar does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArraySetMalformedStructNoMutationTest,
    "PinWright.container.array.set.MalformedStructNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArraySetMalformedStructNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient struct-array handler host created"), Host);
    if (!Host)
    {
        return false;
    }

    FTestMalformedArrayStruct Existing;
    Existing.FirstValue = 11;
    Existing.SecondValue = 22;
    Host->StructArray.Add(Existing);

    TSharedPtr<FJsonObject> StructValue = MakeShared<FJsonObject>();
    StructValue->SetNumberField(TEXT("FirstValue"), 31);
    StructValue->SetStringField(TEXT("SecondValue"), TEXT("not-a-number"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("StructArray"));
    Payload->SetNumberField(TEXT("index"), 0);
    Payload->SetObjectField(TEXT("value"), StructValue);

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("container.array.set"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("container.array.set struct handler registered"), bFound);
    TestFalse(TEXT("container.array.set malformed struct is rejected"), Capture.bSuccess);
    TestEqual(TEXT("container.array.set malformed struct reports UNSUPPORTED_TYPE"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
    TestTrue(TEXT("container.array.set malformed struct error message is non-empty"),
        !Capture.Message.IsEmpty());
    TestEqual(TEXT("container.array.set malformed struct leaves the first field unchanged"),
        Host->StructArray[0].FirstValue, 11);
    TestEqual(TEXT("container.array.set malformed struct leaves the second field unchanged"),
        Host->StructArray[0].SecondValue, 22);
    TestEqual(TEXT("container.array.set malformed struct leaves the array size unchanged"),
        Host->StructArray.Num(), 1);
    TestEqual(TEXT("container.array.set malformed struct does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayAppendMalformedScalarNoMutationTest,
    "PinWright.container.array.append.MalformedScalarNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayAppendMalformedScalarNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient array append host created"), Host);
    if (!Host)
    {
        return false;
    }
    Host->IntArray.Add(77);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntArray"));
    Payload->SetStringField(TEXT("value"), TEXT("not-a-number"));

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("container.array.append"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("container.array.append handler registered"), bFound);
    TestFalse(TEXT("container.array.append malformed scalar is rejected"), Capture.bSuccess);
    TestEqual(TEXT("container.array.append reports UNSUPPORTED_TYPE"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
    TestTrue(TEXT("container.array.append error message is non-empty"), !Capture.Message.IsEmpty());
    TestEqual(TEXT("container.array.append leaves the existing element unchanged"),
        Host->IntArray[0], 77);
    TestEqual(TEXT("container.array.append leaves the array size unchanged"),
        Host->IntArray.Num(), 1);
    TestEqual(TEXT("container.array.append malformed scalar does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayInsertMalformedScalarNoMutationTest,
    "PinWright.container.array.insert.MalformedScalarNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayInsertMalformedScalarNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient array insert host created"), Host);
    if (!Host)
    {
        return false;
    }
    Host->IntArray.Add(77);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntArray"));
    Payload->SetNumberField(TEXT("index"), 0);
    Payload->SetStringField(TEXT("value"), TEXT("not-a-number"));

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("container.array.insert"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("container.array.insert handler registered"), bFound);
    TestFalse(TEXT("container.array.insert malformed scalar is rejected"), Capture.bSuccess);
    TestEqual(TEXT("container.array.insert reports UNSUPPORTED_TYPE"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
    TestTrue(TEXT("container.array.insert error message is non-empty"), !Capture.Message.IsEmpty());
    TestEqual(TEXT("container.array.insert leaves the existing element unchanged"),
        Host->IntArray[0], 77);
    TestEqual(TEXT("container.array.insert leaves the array size unchanged"),
        Host->IntArray.Num(), 1);
    TestEqual(TEXT("container.array.insert malformed scalar does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapSetMalformedScalarNoMutationTest,
    "PinWright.container.map.set.MalformedScalarRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapSetMalformedScalarNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient map handler host created"), Host);
    if (!Host)
    {
        return false;
    }
    Host->IntMap.Add(TEXT("alpha"), 9);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("IntMap"));
    Payload->SetStringField(TEXT("key"), TEXT("alpha"));
    Payload->SetStringField(TEXT("value"), TEXT("not-a-number"));

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("container.map.set"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("container.map.set handler registered"), bFound);
    TestFalse(TEXT("container.map.set malformed scalar is rejected"), Capture.bSuccess);
    TestEqual(TEXT("container.map.set reports UNSUPPORTED_VALUE_TYPE"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_VALUE_TYPE")));
    TestTrue(TEXT("container.map.set error message is non-empty"), !Capture.Message.IsEmpty());
    TestEqual(TEXT("container.map.set leaves the map size unchanged"), Host->IntMap.Num(), 1);
    const int32* ExistingValue = Host->IntMap.Find(TEXT("alpha"));
    TestNotNull(TEXT("container.map.set preserves the existing key"), ExistingValue);
    if (ExistingValue)
    {
        TestEqual(TEXT("container.map.set leaves the existing value unchanged"), *ExistingValue, 9);
    }
    TestEqual(TEXT("container.map.set malformed scalar does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerSetAddMalformedFloatNoMutationTest,
    "PinWright.container.set.add.MalformedFloatRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerSetAddMalformedFloatNoMutationTest::RunTest(const FString& Parameters)
{
    UTestPropertyImportMalformedScalarsHost* Host =
        NewObject<UTestPropertyImportMalformedScalarsHost>(GetTransientPackage());
    TestNotNull(TEXT("Transient set handler host created"), Host);
    if (!Host)
    {
        return false;
    }
    Host->FloatSet.Add(3.5f);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Host->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("FloatSet"));
    Payload->SetStringField(TEXT("value"), TEXT("not-a-number"));

    int32 ModifiedCount = 0;
    const FDelegateHandle ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [Host, &ModifiedCount](UObject* ModifiedObject)
        {
            if (ModifiedObject == Host)
            {
                ++ModifiedCount;
            }
        });

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("container.set.add"), Payload, Capture);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);

    TestTrue(TEXT("container.set.add handler registered"), bFound);
    TestFalse(TEXT("container.set.add malformed float is rejected"), Capture.bSuccess);
    TestEqual(TEXT("container.set.add reports UNSUPPORTED_TYPE"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
    TestTrue(TEXT("container.set.add error message is non-empty"), !Capture.Message.IsEmpty());
    TestEqual(TEXT("container.set.add leaves the set size unchanged"), Host->FloatSet.Num(), 1);
    TestTrue(TEXT("container.set.add preserves the existing float"), Host->FloatSet.Contains(3.5f));
    TestEqual(TEXT("container.set.add malformed float does not broadcast OnObjectModified"),
        ModifiedCount, 0);
    return true;
}
