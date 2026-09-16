// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "AssetDumpFixtureHelpers.h"
#include "TestAssetDumpObjectRefsFixture.h"
#include "Engine/Texture2D.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPtr.h"

namespace
{
    using AssetDumpFixtureHelpers::MakeFixture;

    // Loads a known-present engine texture so tests don't depend on game content.
    UTexture2D* LoadDefaultTexture()
    {
        return LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    }

    FProperty* FindProp(const TCHAR* Name)
    {
        return UTestAssetDumpObjectRefsFixture::StaticClass()->FindPropertyByName(FName(Name));
    }
}

// ============================================================================
// AssetDumpObjectRefs.HardObjectRefNonNull
// TObjectPtr<UTexture2D> populated -> serialized value equals Texture->GetPathName().
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpObjectRefsHardObjectRefNonNullTest,
    "PinWright.utils.asset_dump_object_refs.HardObjectRefNonNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpObjectRefsHardObjectRefNonNullTest::RunTest(const FString& Parameters)
{
    UTexture2D* Tex = LoadDefaultTexture();
    TestNotNull(TEXT("DefaultTexture loaded"), Tex);
    if (!Tex) return false;

    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;
    Fix->HardObjectRef = Tex;

    FProperty* Prop = FindProp(TEXT("HardObjectRef"));
    TestNotNull(TEXT("HardObjectRef property found"), Prop);
    if (!Prop) return false;

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Hard object ref serializes as path string"),
        Val->Type, EJson::String);
    TestEqual(TEXT("Path matches Texture->GetPathName()"),
        Val->AsString(), Tex->GetPathName());
    return true;
}

// ============================================================================
// AssetDumpObjectRefs.HardObjectRefNull
// TObjectPtr<UTexture2D> with no value -> EJson::Null.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpObjectRefsHardObjectRefNullTest,
    "PinWright.utils.asset_dump_object_refs.HardObjectRefNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpObjectRefsHardObjectRefNullTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;
    Fix->HardObjectRef = nullptr;

    FProperty* Prop = FindProp(TEXT("HardObjectRef"));
    if (!Prop) return false;

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Null hard object ref serializes as EJson::Null"),
        Val->Type, EJson::Null);
    return true;
}

// ============================================================================
// AssetDumpObjectRefs.SubclassOfClass
// TSubclassOf<UObject> set to UTexture2D::StaticClass() -> path string equals
// UTexture2D::StaticClass()->GetPathName().
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpObjectRefsSubclassOfClassTest,
    "PinWright.utils.asset_dump_object_refs.SubclassOfClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpObjectRefsSubclassOfClassTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;
    Fix->ClassRef = UTexture2D::StaticClass();

    FProperty* Prop = FindProp(TEXT("ClassRef"));
    TestNotNull(TEXT("ClassRef property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FClassProperty"),
        CastField<FClassProperty>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Class ref serializes as string"), Val->Type, EJson::String);
    TestEqual(TEXT("Path matches UTexture2D::StaticClass()->GetPathName()"),
        Val->AsString(), UTexture2D::StaticClass()->GetPathName());
    return true;
}

// ============================================================================
// AssetDumpObjectRefs.ArrayOfObjectsMixed
// TArray<TObjectPtr<UTexture2D>> with [Texture, nullptr, Texture]:
// - length 3
// - indices 0 and 2 are strings
// - index 1 is EJson::Null
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpObjectRefsArrayOfObjectsMixedTest,
    "PinWright.utils.asset_dump_object_refs.ArrayOfObjectsMixed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpObjectRefsArrayOfObjectsMixedTest::RunTest(const FString& Parameters)
{
    UTexture2D* Tex = LoadDefaultTexture();
    if (!Tex) return false;

    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;
    Fix->ObjectArray.Add(Tex);
    Fix->ObjectArray.Add(nullptr);
    Fix->ObjectArray.Add(Tex);

    FProperty* Prop = FindProp(TEXT("ObjectArray"));
    if (!Prop) return false;

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Array type"), Val->Type, EJson::Array);
    const TArray<TSharedPtr<FJsonValue>>& Arr = Val->AsArray();
    TestEqual(TEXT("Array length is 3"), Arr.Num(), 3);
    if (Arr.Num() != 3) return false;
    TestEqual(TEXT("Index 0 is string"), Arr[0]->Type, EJson::String);
    TestEqual(TEXT("Index 0 path matches"), Arr[0]->AsString(), Tex->GetPathName());
    TestEqual(TEXT("Index 1 is null"), Arr[1]->Type, EJson::Null);
    TestEqual(TEXT("Index 2 is string"), Arr[2]->Type, EJson::String);
    TestEqual(TEXT("Index 2 path matches"), Arr[2]->AsString(), Tex->GetPathName());
    return true;
}

// ============================================================================
// AssetDumpObjectRefs.MapWithObjectValue
// TMap<FString, TObjectPtr<UTexture2D>> -> object with string-path values.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpObjectRefsMapWithObjectValueTest,
    "PinWright.utils.asset_dump_object_refs.MapWithObjectValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpObjectRefsMapWithObjectValueTest::RunTest(const FString& Parameters)
{
    UTexture2D* Tex = LoadDefaultTexture();
    if (!Tex) return false;

    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;
    Fix->ObjectMap.Add(TEXT("first"), Tex);
    Fix->ObjectMap.Add(TEXT("second"), Tex);

    FProperty* Prop = FindProp(TEXT("ObjectMap"));
    if (!Prop) return false;

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Map serializes as object"), Val->Type, EJson::Object);
    const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
    TestNotNull(TEXT("Object is valid"), Obj.Get());
    if (!Obj) return false;

    FString FirstStr;
    TestTrue(TEXT("'first' is string field"),
        Obj->TryGetStringField(TEXT("first"), FirstStr));
    TestEqual(TEXT("'first' path matches"), FirstStr, Tex->GetPathName());

    FString SecondStr;
    TestTrue(TEXT("'second' is string field"),
        Obj->TryGetStringField(TEXT("second"), SecondStr));
    TestEqual(TEXT("'second' path matches"), SecondStr, Tex->GetPathName());
    return true;
}

// ============================================================================
// AssetDumpObjectRefs.UnsupportedKindEmitsTypedMarker
// FFieldPathProperty serializes as a typed marker object with a "_kind" field.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpObjectRefsUnsupportedKindEmitsTypedMarkerTest,
    "PinWright.utils.asset_dump_object_refs.UnsupportedKindEmitsTypedMarker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpObjectRefsUnsupportedKindEmitsTypedMarkerTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpObjectRefsFixture* Fix = MakeFixture();
    if (!Fix) return false;

    FProperty* Prop = FindProp(TEXT("FieldPathRef"));
    TestNotNull(TEXT("FieldPathRef property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FFieldPathProperty"),
        CastField<FFieldPathProperty>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Typed marker is an object"), Val->Type, EJson::Object);
    const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
    TestNotNull(TEXT("Marker object is valid"), Obj.Get());
    if (!Obj) return false;

    FString Kind;
    TestTrue(TEXT("'_kind' field present"), Obj->TryGetStringField(TEXT("_kind"), Kind));
    TestEqual(TEXT("'_kind' identifies field-path property"),
        Kind, FString(TEXT("FFieldPathProperty")));
    return true;
}
