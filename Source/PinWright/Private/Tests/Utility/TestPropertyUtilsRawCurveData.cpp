// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "TestPropertyUtilsRawCurveData.h"

#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"


namespace
{
    FString SerializeJsonValue(const TSharedPtr<FJsonValue>& JsonValue)
    {
        FString Output;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
        if (JsonValue.IsValid())
        {
            FJsonSerializer::Serialize(JsonValue, FString(), Writer);
        }
        return Output;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsRawCurveDataSummaryTest,
    "PinWright.utils.property_utils.RawCurveDataSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsRawCurveDataSummaryTest::RunTest(const FString& Parameters)
{
    UTestRawCurveDataHost* Host = NewObject<UTestRawCurveDataHost>(GetTransientPackage());
    TestNotNull(TEXT("Host UObject created"), Host);
    if (!Host) return false;

    Host->RawCurveData.AddFloatCurveKey(TEXT("Throttle"), AACF_DefaultCurve, 0.0f, 1.0f);
    Host->RawCurveData.AddFloatCurveKey(TEXT("Throttle"), AACF_DefaultCurve, 0.5f, 2.0f);
    Host->RawCurveData.AddFloatCurveKey(TEXT("Throttle"), AACF_DefaultCurve, 1.0f, 3.0f);
    Host->RawCurveData.AddFloatCurveKey(TEXT("Yaw"), AACF_DefaultCurve, 0.25f, -4.0f);

    FProperty* RawCurveDataProp = UTestRawCurveDataHost::StaticClass()->FindPropertyByName(TEXT("RawCurveData"));
    TestNotNull(TEXT("RawCurveData property resolved"), RawCurveDataProp);
    if (!RawCurveDataProp) return false;

    TSharedPtr<FJsonValue> JsonValue = ExportPropertyToJsonValue(Host, RawCurveDataProp);
    TestTrue(TEXT("ExportPropertyToJsonValue returned a value"), JsonValue.IsValid());
    if (!JsonValue.IsValid()) return false;

    const TSharedPtr<FJsonObject>* SummaryObjPtr = nullptr;
    TestTrue(TEXT("RawCurveData exports as a JSON object"), JsonValue->TryGetObject(SummaryObjPtr));
    if (!SummaryObjPtr || !(*SummaryObjPtr)) return false;

    const TSharedPtr<FJsonObject>& SummaryObj = *SummaryObjPtr;
    TestEqual(TEXT("_kind"), SummaryObj->GetStringField(TEXT("_kind")), FString(TEXT("FRawCurveTracks")));
    TestEqual(TEXT("floatCurveCount"), SummaryObj->GetNumberField(TEXT("floatCurveCount")), 2.0);
    TestEqual(TEXT("totalKeyCount"), SummaryObj->GetNumberField(TEXT("totalKeyCount")), 4.0);

    TestEqual(TEXT("transformCurveCount"), SummaryObj->GetNumberField(TEXT("transformCurveCount")), 0.0);

    const TArray<TSharedPtr<FJsonValue>>* FloatCurves = nullptr;
    TestTrue(TEXT("floatCurves array exists"), SummaryObj->TryGetArrayField(TEXT("floatCurves"), FloatCurves));
    if (!FloatCurves) return false;

    TSharedPtr<FJsonObject> ThrottleCurveObj;
    for (const TSharedPtr<FJsonValue>& CurveValue : *FloatCurves)
    {
        const TSharedPtr<FJsonObject>* CurveObjPtr = nullptr;
        if (CurveValue.IsValid() && CurveValue->TryGetObject(CurveObjPtr) && CurveObjPtr && (*CurveObjPtr))
        {
            if ((*CurveObjPtr)->GetStringField(TEXT("name")) == TEXT("Throttle"))
            {
                ThrottleCurveObj = *CurveObjPtr;
                break;
            }
        }
    }

    TestTrue(TEXT("Throttle curve summary found"), ThrottleCurveObj.IsValid());
    if (!ThrottleCurveObj.IsValid()) return false;

    TestEqual(TEXT("Throttle curve type"), ThrottleCurveObj->GetStringField(TEXT("type")), FString(TEXT("float")));
    TestEqual(TEXT("Throttle keyCount"), ThrottleCurveObj->GetNumberField(TEXT("keyCount")), 3.0);

    const TSharedPtr<FJsonObject>* FirstKeyObjPtr = nullptr;
    const TSharedPtr<FJsonObject>* LastKeyObjPtr = nullptr;
    TestTrue(TEXT("firstKey summary exists"), ThrottleCurveObj->TryGetObjectField(TEXT("firstKey"), FirstKeyObjPtr));
    TestTrue(TEXT("lastKey summary exists"), ThrottleCurveObj->TryGetObjectField(TEXT("lastKey"), LastKeyObjPtr));
    if (!FirstKeyObjPtr || !LastKeyObjPtr || !(*FirstKeyObjPtr) || !(*LastKeyObjPtr)) return false;

    TestEqual(TEXT("firstKey.time"), (*FirstKeyObjPtr)->GetNumberField(TEXT("time")), 0.0);
    TestEqual(TEXT("firstKey.value"), (*FirstKeyObjPtr)->GetNumberField(TEXT("value")), 1.0);
    TestEqual(TEXT("lastKey.time"), (*LastKeyObjPtr)->GetNumberField(TEXT("time")), 1.0);
    TestEqual(TEXT("lastKey.value"), (*LastKeyObjPtr)->GetNumberField(TEXT("value")), 3.0);

    const FString Serialized = SerializeJsonValue(JsonValue);
    TestFalse(TEXT("Serialized summary omits ExportText Keys blob"), Serialized.Contains(TEXT("Keys=")));
    TestFalse(TEXT("Serialized summary omits ExportText FloatCurves blob"), Serialized.Contains(TEXT("FloatCurves=(")));

    return true;
}
