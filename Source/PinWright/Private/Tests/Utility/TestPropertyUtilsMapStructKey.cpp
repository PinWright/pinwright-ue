// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for PropertyUtils.cpp::ExportPropertyToJsonValue's TMap key
// fallback. Before the fix, any KeyProp not in {FStrProperty, FNameProperty,
// FIntProperty} fell through to FString::Printf(TEXT("key_%d"), i), producing
// placeholder keys "key_0", "key_1", ... for FGuid / FSoftObjectPath / struct /
// int64 / byte-enum maps. The fix routes through ExportTextItem_Direct so the
// engine emits canonical text for the actual key.
#include "TestPropertyUtilsMapStructKey.h"
#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsMapStructKeyExportTest,
    "PinWright.utils.property_utils.MapStructKeyExport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsMapStructKeyExportTest::RunTest(const FString& Parameters)
{
    UTestMapStructKeyHost* Host = NewObject<UTestMapStructKeyHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    // Two deterministic FGuids so the canonical text form is stable across runs.
    const FGuid GuidA(0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u);
    const FGuid GuidB(0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu);
    Host->GuidToInt.Add(GuidA, 100);
    Host->GuidToInt.Add(GuidB, 200);

    FProperty* MapProp = UTestMapStructKeyHost::StaticClass()->FindPropertyByName(TEXT("GuidToInt"));
    TestNotNull(TEXT("GuidToInt FMapProperty resolved"), MapProp);
    if (!MapProp) return false;

    TSharedPtr<FJsonValue> JsonValue = ExportPropertyToJsonValue(Host, MapProp);
    TestTrue(TEXT("ExportPropertyToJsonValue returned a value"), JsonValue.IsValid());
    if (!JsonValue.IsValid()) return false;

    const TSharedPtr<FJsonObject>* MapObjPtr = nullptr;
    TestTrue(TEXT("Exported value is a JSON object"), JsonValue->TryGetObject(MapObjPtr));
    if (!MapObjPtr || !(*MapObjPtr)) return false;

    const TSharedPtr<FJsonObject>& MapObj = *MapObjPtr;

    // Counterfactual: if PropertyUtils.cpp:560-563 is reverted to FString::Printf(TEXT("key_%d"), i),
    // the JSON key becomes "key_0" instead of the GUID's canonical text form.
    const FString CanonicalA = GuidA.ToString(EGuidFormats::Digits);
    const FString CanonicalB = GuidB.ToString(EGuidFormats::Digits);

    TestEqual(TEXT("Map has exactly 2 entries"), MapObj->Values.Num(), 2);
    TestTrue(FString::Printf(TEXT("GuidA canonical key '%s' present"), *CanonicalA),
        MapObj->HasField(CanonicalA));
    TestTrue(FString::Printf(TEXT("GuidB canonical key '%s' present"), *CanonicalB),
        MapObj->HasField(CanonicalB));
    TestFalse(TEXT("Placeholder key 'key_0' is absent"), MapObj->HasField(TEXT("key_0")));
    TestFalse(TEXT("Placeholder key 'key_1' is absent"), MapObj->HasField(TEXT("key_1")));

    double ValueA = 0.0;
    double ValueB = 0.0;
    if (MapObj->TryGetNumberField(CanonicalA, ValueA))
    {
        TestEqual(TEXT("GuidA -> 100"), ValueA, 100.0);
    }
    if (MapObj->TryGetNumberField(CanonicalB, ValueB))
    {
        TestEqual(TEXT("GuidB -> 200"), ValueB, 200.0);
    }

    return true;
}
