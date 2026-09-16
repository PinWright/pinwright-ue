// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for PropertyUtils.cpp::ExportPropertyToJsonValue's TArray
// struct-element branch. Before the fix, any inner FStructProperty fell through
// to ExportTextItem_Direct, producing EJson::String elements containing UE's
// ExportText grammar "(IntField=...,StringField=...,BoolField=...)". The fix
// routes through StructToJsonObject so each element becomes a proper JSON object.
#include "TestPropertyUtilsArrayStructElement.h"
#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Utils/PropertyInspection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsArrayStructElementExportTest,
    "PinWright.utils.property_utils.ArrayStructElementExport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsArrayStructElementExportTest::RunTest(const FString& Parameters)
{
    UTestArrayStructElementHost* Host = NewObject<UTestArrayStructElementHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    FTestArrayStructElementInner A;
    A.IntField = 42;
    A.StringField = TEXT("hello");
    A.BoolField = true;
    Host->Items.Add(A);

    FTestArrayStructElementInner B;
    B.IntField = -7;
    B.StringField = TEXT("world");
    B.BoolField = false;
    Host->Items.Add(B);

    FProperty* ArrayProp = UTestArrayStructElementHost::StaticClass()->FindPropertyByName(TEXT("Items"));
    TestNotNull(TEXT("Items FProperty resolved"), ArrayProp);
    if (!ArrayProp) return false;

    TSharedPtr<FJsonValue> JsonValue = ExportPropertyToJsonValue(Host, ArrayProp);
    TestTrue(TEXT("ExportPropertyToJsonValue returned a value"), JsonValue.IsValid());
    if (!JsonValue.IsValid()) return false;

    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    TestTrue(TEXT("Exported value is a JSON array"), JsonValue->TryGetArray(Arr));
    if (!Arr) return false;

    TestEqual(TEXT("Array has exactly 2 elements"), Arr->Num(), 2);

    // Counterfactual: if PropertyUtils.cpp's array struct branch is removed, ExportTextItem_Direct fallback runs and elements become EJson::String containing '(IntField=...,StringField=...,BoolField=...)'.

    for (int32 i = 0; i < 2 && i < Arr->Num(); ++i)
    {
        TestTrue(FString::Printf(TEXT("Element %d is a JSON object"), i),
            (*Arr)[i].IsValid() && (*Arr)[i]->Type == EJson::Object);
    }
    if (Arr->Num() < 2) return false;

    // Element 0: {42, "hello", true}
    const TSharedPtr<FJsonObject>* Obj0Ptr = nullptr;
    TestTrue(TEXT("Element 0 TryGetObject"), (*Arr)[0]->TryGetObject(Obj0Ptr));
    if (Obj0Ptr && *Obj0Ptr)
    {
        double IntVal0 = 0.0;
        (*Obj0Ptr)->TryGetNumberField(TEXT("IntField"), IntVal0);
        TestEqual(TEXT("Element 0 IntField == 42"), IntVal0, 42.0);

        FString StrVal0;
        (*Obj0Ptr)->TryGetStringField(TEXT("StringField"), StrVal0);
        TestEqual(TEXT("Element 0 StringField == hello"), StrVal0, FString(TEXT("hello")));

        bool BoolVal0 = false;
        (*Obj0Ptr)->TryGetBoolField(TEXT("BoolField"), BoolVal0);
        TestTrue(TEXT("Element 0 BoolField == true"), BoolVal0);
    }

    // Element 1: {-7, "world", false}
    const TSharedPtr<FJsonObject>* Obj1Ptr = nullptr;
    TestTrue(TEXT("Element 1 TryGetObject"), (*Arr)[1]->TryGetObject(Obj1Ptr));
    if (Obj1Ptr && *Obj1Ptr)
    {
        double IntVal1 = 0.0;
        (*Obj1Ptr)->TryGetNumberField(TEXT("IntField"), IntVal1);
        TestEqual(TEXT("Element 1 IntField == -7"), IntVal1, -7.0);

        FString StrVal1;
        (*Obj1Ptr)->TryGetStringField(TEXT("StringField"), StrVal1);
        TestEqual(TEXT("Element 1 StringField == world"), StrVal1, FString(TEXT("world")));

        bool BoolVal1 = true;
        (*Obj1Ptr)->TryGetBoolField(TEXT("BoolField"), BoolVal1);
        TestFalse(TEXT("Element 1 BoolField == false"), BoolVal1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsSingleStructExportTest,
    "PinWright.utils.property_utils.SingleStructExport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsSingleStructExportTest::RunTest(const FString& Parameters)
{
    USceneComponent* Component = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient SceneComponent created"), Component);
    if (!Component) return false;

    FProperty* TickProp = UActorComponent::StaticClass()->FindPropertyByName(TEXT("PrimaryComponentTick"));
    TestNotNull(TEXT("UActorComponent has PrimaryComponentTick"), TickProp);
    if (!TickProp) return false;

    TSharedPtr<FJsonValue> TickJson = ExportPropertyToJsonValue(Component, TickProp);
    TestTrue(TEXT("PrimaryComponentTick export returned a value"), TickJson.IsValid());
    if (!TickJson.IsValid()) return false;

    const TSharedPtr<FJsonObject>* TickObjPtr = nullptr;
    TestTrue(TEXT("PrimaryComponentTick exports as JSON object"), TickJson->TryGetObject(TickObjPtr));
    if (!TickObjPtr || !(*TickObjPtr)) return false;

    TestTrue(TEXT("Reflected tick struct includes bCanEverTick"),
        (*TickObjPtr)->HasField(TEXT("bCanEverTick")));
    TestTrue(TEXT("Reflected tick struct includes TickInterval"),
        (*TickObjPtr)->HasField(TEXT("TickInterval")));

    FProperty* RelLocProp = USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
    TestNotNull(TEXT("USceneComponent has RelativeLocation"), RelLocProp);
    if (!RelLocProp) return false;

    TSharedPtr<FJsonValue> RelLocJson = ExportPropertyToJsonValue(Component, RelLocProp);
    TestTrue(TEXT("RelativeLocation export returned a value"), RelLocJson.IsValid());
    if (!RelLocJson.IsValid()) return false;

    const TArray<TSharedPtr<FJsonValue>>* RelLocArray = nullptr;
    TestTrue(TEXT("RelativeLocation keeps compact vector array shape"), RelLocJson->TryGetArray(RelLocArray));
    if (!RelLocArray) return false;
    TestEqual(TEXT("RelativeLocation has three elements"), RelLocArray->Num(), 3);

    return true;
}

// Regression test for the array-element subscript support added to
// Utils/PropertyInspection.cpp::ResolveNestedPropertyPath. Before the fix the
// resolver split the path only on '.', looked up each segment as a named
// FProperty, and traversed only struct/object hops — so "Items[1].IntField"
// failed PROPERTY_NOT_FOUND on the literal segment "Items[1]" and a bare
// numeric "Items.1.IntField" failed "Cannot traverse into property 'Items' of
// type 'ArrayProperty'". This test drives the SAME production resolver the
// property.get/set, actor.get_component_property, and SCS/widget/blueprint/
// niagara/anim/gameplaytag callers share, asserting both the bracket form
// (Foo[N]) and the dotted-numeric form (Foo.N) resolve a struct array element's
// leaf field, and that out-of-range / non-array / bad-subscript inputs report a
// clean error instead of resolving. If the [N]/.N. element-stepping is reverted,
// the bracket/dotted reads below resolve to null and these assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveNestedPropertyPathArrayIndexTest,
    "PinWright.utils.property_inspection.ArrayIndexNestedPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveNestedPropertyPathArrayIndexTest::RunTest(const FString& Parameters)
{
    UTestArrayStructElementHost* Host = NewObject<UTestArrayStructElementHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    FTestArrayStructElementInner A;
    A.IntField = 42;
    A.StringField = TEXT("hello");
    A.BoolField = true;
    Host->Items.Add(A);

    FTestArrayStructElementInner B;
    B.IntField = -7;
    B.StringField = TEXT("world");
    B.BoolField = false;
    Host->Items.Add(B);

    // --- Bracket form: Items[1].IntField resolves the int leaf of element 1 ---
    {
        void* Container = nullptr;
        FString Error;
        FProperty* Leaf = ResolveNestedPropertyPath(Host, TEXT("Items[1].IntField"), Container, Error);
        TestNotNull(TEXT("Items[1].IntField resolves a leaf FProperty"), Leaf);
        TestTrue(TEXT("Items[1].IntField yields a container"), Container != nullptr);
        TestEqual(TEXT("Items[1].IntField has no error"), Error, FString());
        if (Leaf && Container)
        {
            TestNotNull(TEXT("Leaf is an FIntProperty"), CastField<FIntProperty>(Leaf));
            TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(Container, Leaf);
            TestTrue(TEXT("Leaf export valid"), Value.IsValid());
            double Num = 0.0;
            if (Value.IsValid()) Value->TryGetNumber(Num);
            TestEqual(TEXT("Items[1].IntField == -7"), Num, -7.0);
        }
    }

    // --- Dotted-numeric form: Items.0.StringField resolves the string leaf ---
    {
        void* Container = nullptr;
        FString Error;
        FProperty* Leaf = ResolveNestedPropertyPath(Host, TEXT("Items.0.StringField"), Container, Error);
        TestNotNull(TEXT("Items.0.StringField resolves a leaf FProperty"), Leaf);
        TestTrue(TEXT("Items.0.StringField yields a container"), Container != nullptr);
        TestEqual(TEXT("Items.0.StringField has no error"), Error, FString());
        if (Leaf && Container)
        {
            TestNotNull(TEXT("Leaf is an FStrProperty"), CastField<FStrProperty>(Leaf));
            TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(Container, Leaf);
            FString Str;
            if (Value.IsValid()) Value->TryGetString(Str);
            TestEqual(TEXT("Items.0.StringField == hello"), Str, FString(TEXT("hello")));
        }
    }

    // --- Bracket form addressing the whole struct element: Items[1] is the struct ---
    {
        void* Container = nullptr;
        FString Error;
        FProperty* Leaf = ResolveNestedPropertyPath(Host, TEXT("Items[0]"), Container, Error);
        TestNotNull(TEXT("Items[0] resolves the inner struct property"), Leaf);
        TestTrue(TEXT("Items[0] yields a container"), Container != nullptr);
        TestNotNull(TEXT("Items[0] leaf is an FStructProperty"), CastField<FStructProperty>(Leaf));
    }

    // --- Out-of-range index reports an error, does not resolve ---
    {
        void* Container = nullptr;
        FString Error;
        FProperty* Leaf = ResolveNestedPropertyPath(Host, TEXT("Items[5].IntField"), Container, Error);
        TestNull(TEXT("Out-of-range index resolves to null"), Leaf);
        TestFalse(TEXT("Out-of-range index populates an error"), Error.IsEmpty());
        TestTrue(TEXT("Error mentions out of range"), Error.Contains(TEXT("out of range")));
    }

    // --- Indexing a non-array property reports an error ---
    {
        // BoolField is reached via element 0 then its bool member; indexing it is invalid.
        void* Container = nullptr;
        FString Error;
        FProperty* Leaf = ResolveNestedPropertyPath(Host, TEXT("Items[0].IntField[0]"), Container, Error);
        TestNull(TEXT("Indexing a non-array leaf resolves to null"), Leaf);
        TestFalse(TEXT("Indexing a non-array leaf populates an error"), Error.IsEmpty());
    }

    // --- A bare array hop without an index still reports a helpful error ---
    {
        void* Container = nullptr;
        FString Error;
        // Items.IntField: IntField is not numeric, so the array hop has no index.
        FProperty* Leaf = ResolveNestedPropertyPath(Host, TEXT("Items.IntField"), Container, Error);
        TestNull(TEXT("Array hop without index resolves to null"), Leaf);
        TestFalse(TEXT("Array hop without index populates an error"), Error.IsEmpty());
    }

    return true;
}
