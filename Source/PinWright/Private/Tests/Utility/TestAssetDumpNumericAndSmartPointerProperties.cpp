// Copyright (c) 2026 Alexander Penkin. MIT License.

// Counterfactual: reverting any per-type PropertyUtils dispatch flips the produced JSON value from a primitive/string/null to the unsupported-sentinel object, failing the matching EJson type assertion; reverting the CPF_Transient skip in BuildClassPropertyJson lets TransientCounter slip through, failing the negative HasField assertion.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "TestAssetDumpNumericFixture.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#include "TestAssetDumpOptionalFixture.h"
#endif
#include "Engine/Texture2D.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace TestAssetDumpNumericAndSmartPointerHelpers
{
    UTestAssetDumpNumericFixture* MakeFixture()
    {
        return NewObject<UTestAssetDumpNumericFixture>(GetTransientPackage());
    }

    UTexture2D* LoadDefaultTexture()
    {
        return LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    }

    FProperty* FindProp(const TCHAR* Name)
    {
        return UTestAssetDumpNumericFixture::StaticClass()->FindPropertyByName(FName(Name));
    }
}

// File-scope `using` declarations would leak these names into the Unity-merged
// translation unit and collide with the same-named helpers defined in sibling
// test files (e.g. TestAssetDumpObjectRefs.cpp). Keep the names namespace-qualified
// at each call site below instead.

// ============================================================================
// UInt32
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpNumericUInt32Test,
    "PinWright.utils.asset_dump_numeric_and_smartptr.UInt32",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpNumericUInt32Test::RunTest(const FString& Parameters)
{
    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;
    Fix->U32 = 12345u;

    FProperty* Prop = TestAssetDumpNumericAndSmartPointerHelpers::FindProp(TEXT("U32"));
    TestNotNull(TEXT("U32 property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FUInt32Property"), CastField<FUInt32Property>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("uint32 serializes as number"), Val->Type, EJson::Number);
    TestEqual(TEXT("uint32 value preserved"), (uint32)Val->AsNumber(), 12345u);
    return true;
}

// ============================================================================
// Int8
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpNumericInt8Test,
    "PinWright.utils.asset_dump_numeric_and_smartptr.Int8",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpNumericInt8Test::RunTest(const FString& Parameters)
{
    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;
    Fix->I8 = -42;

    FProperty* Prop = TestAssetDumpNumericAndSmartPointerHelpers::FindProp(TEXT("I8"));
    TestNotNull(TEXT("I8 property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FInt8Property"), CastField<FInt8Property>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("int8 serializes as number"), Val->Type, EJson::Number);
    TestEqual(TEXT("int8 value preserved"), (int32)Val->AsNumber(), -42);
    return true;
}

// ============================================================================
// UInt16
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpNumericUInt16Test,
    "PinWright.utils.asset_dump_numeric_and_smartptr.UInt16",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpNumericUInt16Test::RunTest(const FString& Parameters)
{
    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;
    Fix->U16 = 60000u;

    FProperty* Prop = TestAssetDumpNumericAndSmartPointerHelpers::FindProp(TEXT("U16"));
    TestNotNull(TEXT("U16 property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FUInt16Property"), CastField<FUInt16Property>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("uint16 serializes as number"), Val->Type, EJson::Number);
    TestEqual(TEXT("uint16 value preserved"), (uint32)Val->AsNumber(), 60000u);
    return true;
}

// ============================================================================
// UInt64
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpNumericUInt64Test,
    "PinWright.utils.asset_dump_numeric_and_smartptr.UInt64",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpNumericUInt64Test::RunTest(const FString& Parameters)
{
    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;
    // Stay under 2^53 so the double round-trip is exact.
    Fix->U64 = 1234567890ull;

    FProperty* Prop = TestAssetDumpNumericAndSmartPointerHelpers::FindProp(TEXT("U64"));
    TestNotNull(TEXT("U64 property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FUInt64Property"), CastField<FUInt64Property>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("uint64 serializes as number"), Val->Type, EJson::Number);
    TestEqual(TEXT("uint64 value preserved"), (uint64)Val->AsNumber(), (uint64)1234567890ull);
    return true;
}

// ============================================================================
// WeakObjectRef populated
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWeakObjectRefNonNullTest,
    "PinWright.utils.asset_dump_numeric_and_smartptr.WeakObjectRefNonNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWeakObjectRefNonNullTest::RunTest(const FString& Parameters)
{
    UTexture2D* Tex = TestAssetDumpNumericAndSmartPointerHelpers::LoadDefaultTexture();
    TestNotNull(TEXT("DefaultTexture loaded"), Tex);
    if (!Tex) return false;

    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;
    Fix->WeakRef = Tex;

    FProperty* Prop = TestAssetDumpNumericAndSmartPointerHelpers::FindProp(TEXT("WeakRef"));
    TestNotNull(TEXT("WeakRef property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FWeakObjectProperty"), CastField<FWeakObjectProperty>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Weak ref serializes as path string"), Val->Type, EJson::String);
    TestEqual(TEXT("Path matches Texture->GetPathName()"),
        Val->AsString(), Tex->GetPathName());
    return true;
}

// ============================================================================
// WeakObjectRef null
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWeakObjectRefNullTest,
    "PinWright.utils.asset_dump_numeric_and_smartptr.WeakObjectRefNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWeakObjectRefNullTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;
    Fix->WeakRef = nullptr;

    FProperty* Prop = TestAssetDumpNumericAndSmartPointerHelpers::FindProp(TEXT("WeakRef"));
    if (!Prop) return false;

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Null weak ref serializes as EJson::Null"), Val->Type, EJson::Null);
    return true;
}

// ============================================================================
// LazyObjectRef (null-only assertion — populating a lazy ref reliably from a
// transient fixture requires guid-backed loading).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpLazyObjectRefTest,
    "PinWright.utils.asset_dump_numeric_and_smartptr.LazyObjectRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpLazyObjectRefTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;

    FProperty* Prop = TestAssetDumpNumericAndSmartPointerHelpers::FindProp(TEXT("LazyRef"));
    TestNotNull(TEXT("LazyRef property found"), Prop);
    if (!Prop) return false;
    TestNotNull(TEXT("Property is FLazyObjectProperty"), CastField<FLazyObjectProperty>(Prop));

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    // Unset lazy ref should emit Null rather than the unsupported sentinel.
    TestEqual(TEXT("Unset lazy ref serializes as EJson::Null"), Val->Type, EJson::Null);
    return true;
}

// ============================================================================
// Optional set / unset (UE 5.5+)
// ============================================================================

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpOptionalSetTest,
    "PinWright.utils.asset_dump_numeric_and_smartptr.OptionalSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpOptionalSetTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpOptionalFixture* Fix = NewObject<UTestAssetDumpOptionalFixture>(GetTransientPackage());
    if (!Fix) return false;
    Fix->OptInt = 7;

    FProperty* Prop = UTestAssetDumpOptionalFixture::StaticClass()->FindPropertyByName(FName(TEXT("OptInt")));
    TestNotNull(TEXT("OptInt property found"), Prop);
    if (!Prop) return false;

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Set TOptional emits the inner value as number"),
        Val->Type, EJson::Number);
    TestEqual(TEXT("Inner value preserved"), (int32)Val->AsNumber(), 7);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpOptionalUnsetTest,
    "PinWright.utils.asset_dump_numeric_and_smartptr.OptionalUnset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpOptionalUnsetTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpOptionalFixture* Fix = NewObject<UTestAssetDumpOptionalFixture>(GetTransientPackage());
    if (!Fix) return false;
    Fix->OptInt.Reset();

    FProperty* Prop = UTestAssetDumpOptionalFixture::StaticClass()->FindPropertyByName(FName(TEXT("OptInt")));
    TestNotNull(TEXT("OptInt property found"), Prop);
    if (!Prop) return false;

    TSharedPtr<FJsonValue> Val = ExportPropertyToJsonValue(Fix, Prop);
    TestNotNull(TEXT("Value emitted"), Val.Get());
    if (!Val) return false;
    TestEqual(TEXT("Unset TOptional emits null"), Val->Type, EJson::Null);
    return true;
}

#endif // UE >= 5.5

// ============================================================================
// Transient walk-skip: BuildClassPropertyJson must omit Transient UPROPERTYs
// while keeping non-transient siblings of the same type.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpTransientPropertyWalkSkipTest,
    "PinWright.utils.asset_dump_numeric_and_smartptr.TransientPropertyWalkSkip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpTransientPropertyWalkSkipTest::RunTest(const FString& Parameters)
{
    UTestAssetDumpNumericFixture* Fix = TestAssetDumpNumericAndSmartPointerHelpers::MakeFixture();
    if (!Fix) return false;
    Fix->TransientCounter = 99;
    Fix->PersistentCounter = 100;

    // Passing ParentCDO=nullptr forces every property to be treated as overridden
    // so we exercise the walk's emit path rather than the equality cull.
    TSharedPtr<FJsonObject> Out = BuildClassPropertyJson(Fix, nullptr);
    TestNotNull(TEXT("Result object emitted"), Out.Get());
    if (!Out) return false;

    TestFalse(TEXT("TransientCounter is omitted by the walk"),
        Out->HasField(TEXT("TransientCounter")));
    TestTrue(TEXT("PersistentCounter is emitted by the walk"),
        Out->HasField(TEXT("PersistentCounter")));
    return true;
}
