// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-property-set-object-array-silent-null.
//
// The shared importer's FArrayProperty -> FObjectProperty inner branch
// (PropertyImport.cpp) used to coerce ANY non-string JSON element (a {"$class":...}
// instanced-subobject object, a number, a bool) to an EMPTY path, which
// IsNullObjectSentinel treats as the "clear to null" sentinel — so the element was
// stored as null and the whole apply returned TRUE. A non-loadable string path
// fared no better: it only UE_LOG(Warning)'d and still stored null + returned true.
// property.set therefore reported applied:true with value:[null] — a silent
// success-with-no-effect on the generic reflected writer.
//
// The scalar FObjectProperty branch in the SAME file (665-706) fails loud (returns
// false with "Failed to load object at path" / "Unsupported JSON type for object
// property"). The fix mirrors those guards in the array-inner branch: a
// non-string/non-null element and a non-sentinel path that fails to load now return
// false, while the legitimate clear cases (JSON null and the "", "None", "null"
// string sentinels) still store null and return true.
//
// This drives the real production ApplyJsonValueToProperty against a reflected
// TArray<TObjectPtr<UObject>> fixture. With the fix reverted, the three FAIL-LOUD
// assertions (JSON-object element / unloadable path / number element must return
// false) all flip to true and this test fails.
#include "TestObjectArrayImportFixture.h"
#include "Misc/AutomationTest.h"
#include "Utils/PropertyImport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    // Wrap a single JSON element in a one-element JSON array value — the shape
    // property.set uses to replace a whole object array.
    TSharedPtr<FJsonValue> MakeSingleElementArray(const TSharedPtr<FJsonValue>& Element)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(Element);
        return MakeShared<FJsonValueArray>(Arr);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectArrayImportFailLoudTest,
    "PinWright.utils.property_import.ObjectArrayElementFailLoud",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectArrayImportFailLoudTest::RunTest(const FString& Parameters)
{
    UTestObjectArrayImportHost* Host = NewObject<UTestObjectArrayImportHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(
        UTestObjectArrayImportHost::StaticClass(), TEXT("ObjectList"));
    TestNotNull(TEXT("Fixture has ObjectList FArrayProperty"), ArrayProp);
    if (!ArrayProp) return false;
    // Sanity: the inner really is an object property (the branch under test).
    TestNotNull(TEXT("ObjectList inner is an FObjectProperty"),
        CastField<FObjectProperty>(ArrayProp->Inner));

    // --- FAIL-LOUD case 1: a {"$class":...} instanced-subobject JSON object.
    // The exact repro shape. Pre-fix: returns true, ObjectList == [null].
    {
        TSharedPtr<FJsonObject> Inst = MakeShared<FJsonObject>();
        Inst->SetStringField(TEXT("$class"), TEXT("InputModifierNegate"));
        Inst->SetBoolField(TEXT("bY"), true);
        const TSharedPtr<FJsonValue> Value = MakeSingleElementArray(MakeShared<FJsonValueObject>(Inst));

        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Host, ArrayProp, Value, Err);
        TestFalse(TEXT("JSON-object array element fails loud, not a silent null"), bApplied);
        TestTrue(TEXT("JSON-object element failure carries an error message"), !Err.IsEmpty());
    }

    // --- FAIL-LOUD case 2: a non-loadable asset path string.
    // Pre-fix: UE_LOG(Warning) only, stores null, returns true.
    {
        const TSharedPtr<FJsonValue> Value = MakeSingleElementArray(
            MakeShared<FJsonValueString>(TEXT("/Game/PinWrightTest/DoesNotExist.DoesNotExist")));

        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Host, ArrayProp, Value, Err);
        TestFalse(TEXT("unloadable path array element fails loud"), bApplied);
        TestTrue(TEXT("unloadable path failure carries an error message"), !Err.IsEmpty());
    }

    // --- FAIL-LOUD case 3: a number element (any non-string/non-null JSON type).
    {
        const TSharedPtr<FJsonValue> Value = MakeSingleElementArray(MakeShared<FJsonValueNumber>(123));

        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Host, ArrayProp, Value, Err);
        TestFalse(TEXT("number array element fails loud"), bApplied);
    }

    // --- PRESERVE case 4: a JSON null element is a legitimate "clear to null".
    {
        const TSharedPtr<FJsonValue> Value = MakeSingleElementArray(MakeShared<FJsonValueNull>());

        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Host, ArrayProp, Value, Err);
        TestTrue(FString::Printf(TEXT("JSON-null element clears to null (err='%s')"), *Err), bApplied);
        if (TestEqual(TEXT("null element produced a one-element array"), Host->ObjectList.Num(), 1))
        {
            TestNull(TEXT("null element stored as null"), Host->ObjectList[0].Get());
        }
    }

    // --- PRESERVE case 5: the "None" string sentinel also clears to null.
    {
        const TSharedPtr<FJsonValue> Value = MakeSingleElementArray(MakeShared<FJsonValueString>(TEXT("None")));

        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Host, ArrayProp, Value, Err);
        TestTrue(FString::Printf(TEXT("'None' sentinel clears to null (err='%s')"), *Err), bApplied);
        if (TestEqual(TEXT("sentinel element produced a one-element array"), Host->ObjectList.Num(), 1))
        {
            TestNull(TEXT("'None' sentinel stored as null"), Host->ObjectList[0].Get());
        }
    }

    // --- PRESERVE case 6: a genuinely loadable path still loads and returns true.
    // /Script/CoreUObject.Object is the UObject class object — always resident in
    // any editor process — so the happy path is exercised without an on-disk asset,
    // and this guards the fix against over-rejecting valid paths.
    {
        const TSharedPtr<FJsonValue> Value = MakeSingleElementArray(
            MakeShared<FJsonValueString>(TEXT("/Script/CoreUObject.Object")));

        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Host, ArrayProp, Value, Err);
        TestTrue(FString::Printf(TEXT("loadable path element applies (err='%s')"), *Err), bApplied);
        if (TestEqual(TEXT("loadable element produced a one-element array"), Host->ObjectList.Num(), 1))
        {
            TestNotNull(TEXT("loadable element stored the resolved object"), Host->ObjectList[0].Get());
        }
    }

    return true;
}
