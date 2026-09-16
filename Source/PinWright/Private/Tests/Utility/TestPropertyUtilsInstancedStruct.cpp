// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "TestPropertyUtilsInstancedStruct.h"

#include "Misc/AutomationTest.h"
#include "Utils/PropertyExport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// Regression coverage for B-instanced-struct-export-opaque: an FInstancedStruct
// UPROPERTY (single-valued and inside a TArray) must serialize its type-erased
// inner-struct payload — the inner UScriptStruct's field values plus a `_kind`
// marker naming the inner type — rather than the opaque empty object {} that the
// generic StructToJsonObject field-walk produced before the fix.
//
// Before the fix both assertions on the inner fields fail because the wrapper
// serializes as {} (no IntField/StringField/BoolField, no _kind).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsInstancedStructExportTest,
    "PinWright.utils.property_utils.InstancedStructExport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsInstancedStructExportTest::RunTest(const FString& Parameters)
{
    UTestInstancedStructHost* Host = NewObject<UTestInstancedStructHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    // Single-valued wrapper holding a payload with known field values.
    Host->SingleResult.InitializeAs<FTestInstancedStructPayload>();
    {
        FTestInstancedStructPayload& Payload = Host->SingleResult.GetMutable<FTestInstancedStructPayload>();
        Payload.IntField = 42;
        Payload.StringField = TEXT("hello");
        Payload.BoolField = true;
    }

    // Array of wrappers, each holding a distinct payload.
    {
        FInstancedStruct Elem0;
        Elem0.InitializeAs<FTestInstancedStructPayload>();
        Elem0.GetMutable<FTestInstancedStructPayload>().IntField = 7;
        Elem0.GetMutable<FTestInstancedStructPayload>().StringField = TEXT("row0");
        Host->ArrayResults.Add(Elem0);

        FInstancedStruct Elem1;
        Elem1.InitializeAs<FTestInstancedStructPayload>();
        Elem1.GetMutable<FTestInstancedStructPayload>().IntField = 9;
        Elem1.GetMutable<FTestInstancedStructPayload>().StringField = TEXT("row1");
        Host->ArrayResults.Add(Elem1);
    }

    const FString ExpectedKind = FTestInstancedStructPayload::StaticStruct()->GetStructCPPName();

    // --- Single-valued FInstancedStruct ---
    {
        FProperty* SingleProp = UTestInstancedStructHost::StaticClass()->FindPropertyByName(TEXT("SingleResult"));
        TestNotNull(TEXT("SingleResult property resolved"), SingleProp);
        if (!SingleProp) return false;

        TSharedPtr<FJsonValue> JsonValue = ExportPropertyToJsonValue(Host, SingleProp);
        TestTrue(TEXT("SingleResult exported a value"), JsonValue.IsValid());
        if (!JsonValue.IsValid()) return false;

        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        TestTrue(TEXT("SingleResult exports as a JSON object"), JsonValue->TryGetObject(ObjPtr));
        if (!ObjPtr || !(*ObjPtr)) return false;
        const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

        // The defect: this object was {} before the fix. Inner fields must now be present.
        int32 IntField = 0;
        TestTrue(TEXT("SingleResult.IntField surfaced"), Obj->TryGetNumberField(TEXT("IntField"), IntField));
        TestEqual(TEXT("SingleResult.IntField value"), IntField, 42);
        TestEqual(TEXT("SingleResult.StringField value"), Obj->GetStringField(TEXT("StringField")), FString(TEXT("hello")));
        TestTrue(TEXT("SingleResult.BoolField value"), Obj->GetBoolField(TEXT("BoolField")));
        TestEqual(TEXT("SingleResult._kind names inner type"), Obj->GetStringField(TEXT("_kind")), ExpectedKind);
    }

    // --- TArray<FInstancedStruct> ---
    {
        FProperty* ArrayProp = UTestInstancedStructHost::StaticClass()->FindPropertyByName(TEXT("ArrayResults"));
        TestNotNull(TEXT("ArrayResults property resolved"), ArrayProp);
        if (!ArrayProp) return false;

        TSharedPtr<FJsonValue> JsonValue = ExportPropertyToJsonValue(Host, ArrayProp);
        TestTrue(TEXT("ArrayResults exported a value"), JsonValue.IsValid());
        if (!JsonValue.IsValid()) return false;

        const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
        TestTrue(TEXT("ArrayResults exports as a JSON array"), JsonValue->TryGetArray(ArrPtr));
        if (!ArrPtr) return false;
        TestEqual(TEXT("ArrayResults element count"), ArrPtr->Num(), 2);
        if (ArrPtr->Num() != 2) return false;

        const TSharedPtr<FJsonObject>* Elem0Ptr = nullptr;
        TestTrue(TEXT("ArrayResults[0] is a JSON object"), (*ArrPtr)[0]->TryGetObject(Elem0Ptr));
        if (!Elem0Ptr || !(*Elem0Ptr)) return false;

        // The defect: each element was {} before the fix.
        int32 Elem0Int = 0;
        TestTrue(TEXT("ArrayResults[0].IntField surfaced"), (*Elem0Ptr)->TryGetNumberField(TEXT("IntField"), Elem0Int));
        TestEqual(TEXT("ArrayResults[0].IntField value"), Elem0Int, 7);
        TestEqual(TEXT("ArrayResults[0].StringField value"), (*Elem0Ptr)->GetStringField(TEXT("StringField")), FString(TEXT("row0")));
        TestEqual(TEXT("ArrayResults[0]._kind names inner type"), (*Elem0Ptr)->GetStringField(TEXT("_kind")), ExpectedKind);

        const TSharedPtr<FJsonObject>* Elem1Ptr = nullptr;
        TestTrue(TEXT("ArrayResults[1] is a JSON object"), (*ArrPtr)[1]->TryGetObject(Elem1Ptr));
        if (!Elem1Ptr || !(*Elem1Ptr)) return false;
        TestEqual(TEXT("ArrayResults[1].StringField value"), (*Elem1Ptr)->GetStringField(TEXT("StringField")), FString(TEXT("row1")));
    }

    // --- Empty wrapper (null inner type) stays harmless: _kind null, no crash ---
    {
        UTestInstancedStructHost* EmptyHost = NewObject<UTestInstancedStructHost>(GetTransientPackage());
        FProperty* SingleProp = UTestInstancedStructHost::StaticClass()->FindPropertyByName(TEXT("SingleResult"));
        if (SingleProp)
        {
            TSharedPtr<FJsonValue> JsonValue = ExportPropertyToJsonValue(EmptyHost, SingleProp);
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            TestTrue(TEXT("Empty wrapper still exports a JSON object"), JsonValue.IsValid() && JsonValue->TryGetObject(ObjPtr));
            if (ObjPtr && (*ObjPtr))
            {
                TestTrue(TEXT("Empty wrapper carries a _kind field"), (*ObjPtr)->HasField(TEXT("_kind")));
            }
        }
    }

    return true;
}
