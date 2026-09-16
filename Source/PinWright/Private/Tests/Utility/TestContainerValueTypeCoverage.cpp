// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-container-map-value-type-coverage.
//
// container.map.get/set and container.array.get/set used to (de)serialize only four
// reflected value/element types via a hand-rolled CastField chain: FStr / FInt / FFloat
// / FBool. Every other value type — struct, object, soft-object, FText, FName, enum —
// fell through to "[UNSUPPORTED_VALUE_TYPE] Unsupported map value type." (map) or
// "[UNSUPPORTED_TYPE] Unsupported array element type." (array). On real assets (the live
// repro target RTG_UE4Manny_UE5Manny.TargetRetargetPoses is TMap<FName, FIKRetargetPose>)
// that meant the read/edit-a-container-entry workflow could never complete: every host
// map/array there is struct-/FText-/object-/name-valued.
//
// The fix routes the value/element through the SAME ExportPropertyToJsonValue /
// ApplyJsonValueToProperty path property.get/property.set already use, so the container
// verbs cover whatever the property verbs cover. This test drives the real registered
// handlers end-to-end against a reflected fixture whose value/element types are all
// NON-primitive (struct, enum, FName) and asserts:
//   * map.set of a struct (JSON object) succeeds (pre-fix: UNSUPPORTED_VALUE_TYPE),
//   * map.get returns that struct as a structured JSON object with its fields readable,
//   * map.set/get round-trip an enum value,
//   * array.set/get round-trip a struct element and an FName element.
// With the four-type chain restored, every set below returns UNSUPPORTED_VALUE_TYPE /
// UNSUPPORTED_TYPE and these assertions fail.
#include "TestContainerValueTypeHost.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerValueTypeCoverageTest,
    "PinWright.container.ValueTypeCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerValueTypeCoverageTest::RunTest(const FString& Parameters)
{
    UTestContainerValueTypeHost* Host = NewObject<UTestContainerValueTypeHost>(
        GetTransientPackage(), TEXT("FContainerValueTypeCoverageTest_Host"));
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    const FString ObjectPath = Host->GetPathName();
    TestTrue(TEXT("Host path resolves to same object"),
        FindObject<UObject>(nullptr, *ObjectPath) == Host);

    // -----------------------------------------------------------------------
    // 1. FName -> struct map: set a struct VALUE as a JSON object, read it back
    //    structured. This is the exact shape of the reported repro.
    // -----------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> PoseJson = MakeShared<FJsonObject>();
        PoseJson->SetStringField(TEXT("Label"), TEXT("Audit Probe Pose"));
        PoseJson->SetNumberField(TEXT("Weight"), 42);

        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("objectPath"), ObjectPath);
        SetPayload->SetStringField(TEXT("propertyName"), TEXT("NameToPose"));
        SetPayload->SetStringField(TEXT("key"), TEXT("Probe"));
        SetPayload->SetObjectField(TEXT("value"), PoseJson);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("container.map.set"), SetPayload, Capture);
        TestTrue(TEXT("container.map.set registered"), bFound);
        // Pre-fix: bSuccess == false, ErrorCode == UNSUPPORTED_VALUE_TYPE.
        TestTrue(TEXT("map.set(struct value) succeeded"), Capture.bSuccess);
        TestNotEqual(TEXT("map.set(struct value) is not UNSUPPORTED_VALUE_TYPE"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_VALUE_TYPE")));

        // The value actually landed in the reflected map.
        TestEqual(TEXT("underlying map gained the struct entry"), Host->NameToPose.Num(), 1);
        if (FTestContainerPose* Stored = Host->NameToPose.Find(FName(TEXT("Probe"))))
        {
            TestEqual(TEXT("stored struct Label round-tripped"), Stored->Label, FString(TEXT("Audit Probe Pose")));
            TestEqual(TEXT("stored struct Weight round-tripped"), Stored->Weight, 42);
        }
        else
        {
            AddError(TEXT("map.set(struct value) did not insert key 'Probe'"));
        }
    }

    // map.get must return the struct as a structured JSON OBJECT (not an error, not a string).
    {
        TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
        GetPayload->SetStringField(TEXT("objectPath"), ObjectPath);
        GetPayload->SetStringField(TEXT("propertyName"), TEXT("NameToPose"));
        GetPayload->SetStringField(TEXT("key"), TEXT("Probe"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.map.get"), GetPayload, Capture);
        TestTrue(TEXT("map.get(struct value) succeeded"), Capture.bSuccess);
        TestNotEqual(TEXT("map.get(struct value) is not UNSUPPORTED_VALUE_TYPE"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_VALUE_TYPE")));

        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        const bool bIsObject = Capture.Result.IsValid()
            && Capture.Result->TryGetObjectField(TEXT("value"), ValueObj)
            && ValueObj && (*ValueObj).IsValid();
        TestTrue(TEXT("map.get returns the struct value as a JSON object"), bIsObject);
        if (bIsObject)
        {
            FString Label;
            (*ValueObj)->TryGetStringField(TEXT("Label"), Label);
            TestEqual(TEXT("returned struct exposes Label field"), Label, FString(TEXT("Audit Probe Pose")));
        }
    }

    // -----------------------------------------------------------------------
    // 2. FString -> enum map: set the enum by name, read it back.
    // -----------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("objectPath"), ObjectPath);
        SetPayload->SetStringField(TEXT("propertyName"), TEXT("NameToGrade"));
        SetPayload->SetStringField(TEXT("key"), TEXT("First"));
        SetPayload->SetStringField(TEXT("value"), TEXT("Gold"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.map.set"), SetPayload, Capture);
        TestTrue(TEXT("map.set(enum value) succeeded"), Capture.bSuccess);
        TestNotEqual(TEXT("map.set(enum value) is not UNSUPPORTED_VALUE_TYPE"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_VALUE_TYPE")));
        if (ETestContainerValueGrade* Grade = Host->NameToGrade.Find(FString(TEXT("First"))))
        {
            TestEqual(TEXT("enum value stored as Gold"), (uint8)*Grade, (uint8)ETestContainerValueGrade::Gold);
        }
        else
        {
            AddError(TEXT("map.set(enum value) did not insert key 'First'"));
        }
    }

    // -----------------------------------------------------------------------
    // 3. Array of struct: set element 0 (JSON object), read it back structured.
    // -----------------------------------------------------------------------
    {
        // Seed one default element so index 0 exists (array.set requires an in-range index).
        Host->PoseList.AddDefaulted(1);

        TSharedPtr<FJsonObject> ElemJson = MakeShared<FJsonObject>();
        ElemJson->SetStringField(TEXT("Label"), TEXT("Elem Pose"));
        ElemJson->SetNumberField(TEXT("Weight"), 7);

        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("objectPath"), ObjectPath);
        SetPayload->SetStringField(TEXT("propertyName"), TEXT("PoseList"));
        SetPayload->SetNumberField(TEXT("index"), 0);
        SetPayload->SetObjectField(TEXT("value"), ElemJson);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.array.set"), SetPayload, Capture);
        TestTrue(TEXT("array.set(struct element) succeeded"), Capture.bSuccess);
        TestNotEqual(TEXT("array.set(struct element) is not UNSUPPORTED_TYPE"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
        if (Host->PoseList.Num() == 1)
        {
            TestEqual(TEXT("array struct element Label round-tripped"),
                Host->PoseList[0].Label, FString(TEXT("Elem Pose")));
            TestEqual(TEXT("array struct element Weight round-tripped"),
                Host->PoseList[0].Weight, 7);
        }
    }

    // array.get of the struct element returns a structured JSON object.
    {
        TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
        GetPayload->SetStringField(TEXT("objectPath"), ObjectPath);
        GetPayload->SetStringField(TEXT("propertyName"), TEXT("PoseList"));
        GetPayload->SetNumberField(TEXT("index"), 0);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.array.get"), GetPayload, Capture);
        TestTrue(TEXT("array.get(struct element) succeeded"), Capture.bSuccess);
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        const bool bIsObject = Capture.Result.IsValid()
            && Capture.Result->TryGetObjectField(TEXT("value"), ValueObj)
            && ValueObj && (*ValueObj).IsValid();
        TestTrue(TEXT("array.get returns the struct element as a JSON object"), bIsObject);
    }

    // -----------------------------------------------------------------------
    // 4. Array of FName: set element 0 to an FName, read it back.
    // -----------------------------------------------------------------------
    {
        Host->NameList.AddDefaulted(1);

        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("objectPath"), ObjectPath);
        SetPayload->SetStringField(TEXT("propertyName"), TEXT("NameList"));
        SetPayload->SetNumberField(TEXT("index"), 0);
        SetPayload->SetStringField(TEXT("value"), TEXT("Tag_Alpha"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("container.array.set"), SetPayload, Capture);
        TestTrue(TEXT("array.set(FName element) succeeded"), Capture.bSuccess);
        TestNotEqual(TEXT("array.set(FName element) is not UNSUPPORTED_TYPE"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
        if (Host->NameList.Num() == 1)
        {
            TestEqual(TEXT("array FName element round-tripped"),
                Host->NameList[0], FName(TEXT("Tag_Alpha")));
        }
    }

    // -----------------------------------------------------------------------
    // 5. Array insert of a struct: the sibling container.array.insert verb used a
    //    separate four-primitive CastField ladder that never called
    //    ApplyJsonValueToProperty, so a struct element inner fell through to
    //    CONVERSION_FAILED ("Failed to insert value: unsupported type") — the exact
    //    capability gap set/get/append fixed. The fix routes insert through the shared
    //    ApplyJsonValueToProperty path too. Insert at index 0 of a one-element list
    //    (seeded above with the FName section? no — PoseList already has one element
    //    from section 3) shifts the existing element to index 1 and lands the new struct
    //    at index 0.
    // -----------------------------------------------------------------------
    {
        // PoseList currently holds one element (from section 3). Insert a new struct at
        // index 0; the existing "Elem Pose" struct shifts to index 1.
        const int32 PrevNum = Host->PoseList.Num();

        TSharedPtr<FJsonObject> ElemJson = MakeShared<FJsonObject>();
        ElemJson->SetStringField(TEXT("Label"), TEXT("Inserted Pose"));
        ElemJson->SetNumberField(TEXT("Weight"), 5);

        TSharedPtr<FJsonObject> InsertPayload = MakeShared<FJsonObject>();
        InsertPayload->SetStringField(TEXT("objectPath"), ObjectPath);
        InsertPayload->SetStringField(TEXT("propertyName"), TEXT("PoseList"));
        InsertPayload->SetNumberField(TEXT("index"), 0);
        InsertPayload->SetObjectField(TEXT("value"), ElemJson);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("container.array.insert"), InsertPayload, Capture);
        TestTrue(TEXT("container.array.insert registered"), bFound);
        // Pre-fix: CONVERSION_FAILED on the struct inner (the four-primitive ladder).
        TestTrue(TEXT("array.insert(struct element) succeeded"), Capture.bSuccess);
        TestNotEqual(TEXT("array.insert(struct element) is not CONVERSION_FAILED"),
            Capture.ErrorCode, FString(TEXT("CONVERSION_FAILED")));
        TestNotEqual(TEXT("array.insert(struct element) is not UNSUPPORTED_TYPE"),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));

        TestEqual(TEXT("array.insert grew PoseList by one element"),
            Host->PoseList.Num(), PrevNum + 1);
        if (Host->PoseList.Num() == PrevNum + 1)
        {
            // The inserted struct landed at index 0 (not the object base, not the wrong slot).
            TestEqual(TEXT("inserted struct Label landed at index 0"),
                Host->PoseList[0].Label, FString(TEXT("Inserted Pose")));
            TestEqual(TEXT("inserted struct Weight landed at index 0"),
                Host->PoseList[0].Weight, 5);
            // The previously-set struct shifted to index 1, intact.
            TestEqual(TEXT("pre-existing struct shifted to index 1"),
                Host->PoseList[1].Label, FString(TEXT("Elem Pose")));
        }
    }

    return true;
}

// Regression test for B-container-array-append-struct-element-crash.
//
// container.array.append handed the OBJECT BASE (TargetContainer), not the new element
// pointer (ElemPtr), to ApplyJsonValueToProperty. For a struct inner (e.g. FDirectoryPath /
// the FTestContainerPose fixture below) the converter then wrote the struct's FString
// sub-field at the object's base address using the element-relative offset -> an
// EXCEPTION_ACCESS_VIOLATION in FString::operator= (callstack PropertyImport.cpp:318 <- :828
// <- UtilityPropertyHandler.cpp), killing the editor. (The sibling container.array.set/.get
// were already fixed by F-container-map-value-type-coverage; append was the lone verb still
// passing the object base.) The fix passes ElemPtr, so the write lands in the new element.
//
// This drives the real registered container.array.append handler end-to-end against a
// reflected fixture whose element type is a struct-with-FString and asserts the appended
// value actually lands in the new element. With the bug reverted the append writes to the
// object base instead (or crashes the editor), so PoseList[0] never receives the value and
// the field-round-trip assertions below fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayAppendStructElementTest,
    "PinWright.container.array.append.StructElementNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayAppendStructElementTest::RunTest(const FString& Parameters)
{
    UTestContainerValueTypeHost* Host = NewObject<UTestContainerValueTypeHost>(
        GetTransientPackage(), TEXT("FContainerArrayAppendStructElementTest_Host"));
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    const FString ObjectPath = Host->GetPathName();
    TestTrue(TEXT("Host path resolves to same object"),
        FindObject<UObject>(nullptr, *ObjectPath) == Host);

    // PoseList starts empty; append a struct element as a JSON object. Pre-fix this reaches
    // ApplyJsonValueToProperty with the object base and the struct branch writes the FString
    // sub-field at a mismatched address (AV / memory corruption).
    TSharedPtr<FJsonObject> ElemJson = MakeShared<FJsonObject>();
    ElemJson->SetStringField(TEXT("Label"), TEXT("Appended Pose"));
    ElemJson->SetNumberField(TEXT("Weight"), 13);

    TSharedPtr<FJsonObject> AppendPayload = MakeShared<FJsonObject>();
    AppendPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    AppendPayload->SetStringField(TEXT("propertyName"), TEXT("PoseList"));
    AppendPayload->SetObjectField(TEXT("value"), ElemJson);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("container.array.append"), AppendPayload, Capture);
    TestTrue(TEXT("container.array.append registered"), bFound);
    // Pre-fix: crash before this returns, or CONVERSION_FAILED on the struct inner.
    TestTrue(TEXT("array.append(struct element) succeeded"), Capture.bSuccess);
    TestNotEqual(TEXT("array.append(struct element) is not CONVERSION_FAILED"),
        Capture.ErrorCode, FString(TEXT("CONVERSION_FAILED")));

    // The appended struct actually landed in the new element (index 0). This is the strong
    // regression guard: writing to the object base instead of ElemPtr leaves PoseList[0]
    // default-constructed, so the field checks below fail.
    TestEqual(TEXT("append grew PoseList by one element"), Host->PoseList.Num(), 1);
    if (Host->PoseList.Num() == 1)
    {
        TestEqual(TEXT("appended struct Label round-tripped into the new element"),
            Host->PoseList[0].Label, FString(TEXT("Appended Pose")));
        TestEqual(TEXT("appended struct Weight round-tripped into the new element"),
            Host->PoseList[0].Weight, 13);
    }

    return true;
}
