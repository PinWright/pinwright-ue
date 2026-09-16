// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-mgir-tarray-properties-dropped (struct-inner array
// branch in PropertyUtils::ApplyJsonValueToProperty).
//
// Counterfactual: if the FStructProperty array-inner branch in PropertyUtils.cpp
// is reverted, this assertion fails because ApplyJsonValueToProperty returns
// "Unsupported array inner property type".
#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Materials/MaterialExpressionCustom.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApplyJsonValueToProperty_StructArrayRoundTrip,
    "PinWright.utils.property_utils.ApplyJsonValueToProperty.StructArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FApplyJsonValueToProperty_StructArrayRoundTrip::RunTest(const FString& Parameters)
{
    UMaterialExpressionCustom* Expr = NewObject<UMaterialExpressionCustom>(GetTransientPackage());
    TestNotNull(TEXT("Transient UMaterialExpressionCustom created"), Expr);
    if (!Expr) return false;

    FArrayProperty* AdditionalOutputsProp = CastField<FArrayProperty>(
        UMaterialExpressionCustom::StaticClass()->FindPropertyByName(TEXT("AdditionalOutputs")));
    TestNotNull(TEXT("AdditionalOutputs FArrayProperty present"), AdditionalOutputsProp);
    if (!AdditionalOutputsProp) return false;

    // Build a JSON array of struct-shaped JSON objects. FCustomOutput has
    // OutputName (FName) and OutputType (TEnumAsByte<ECustomMaterialOutputType>).
    // CMOT_Float3 = 2 (header lists Float1, Float2, Float3, ...).
    auto MakeOutput = [](const TCHAR* Name, int32 TypeValue)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("OutputName"), Name);
        Obj->SetNumberField(TEXT("OutputType"), TypeValue);
        return MakeShared<FJsonValueObject>(Obj);
    };

    TArray<TSharedPtr<FJsonValue>> Elements;
    Elements.Add(MakeOutput(TEXT("Alpha"), 0)); // CMOT_Float1
    Elements.Add(MakeOutput(TEXT("RGB"), 2));   // CMOT_Float3
    Elements.Add(MakeOutput(TEXT("RGBA"), 3));  // CMOT_Float4

    TSharedPtr<FJsonValue> JsonArray = MakeShared<FJsonValueArray>(Elements);

    FString Err;
    const bool bApplied = ApplyJsonValueToProperty(Expr, AdditionalOutputsProp, JsonArray, Err);
    TestTrue(FString::Printf(TEXT("Apply returned true (err='%s')"), *Err), bApplied);
    if (!bApplied) return false;

    TestEqual(TEXT("AdditionalOutputs element count"), Expr->AdditionalOutputs.Num(), 3);
    if (Expr->AdditionalOutputs.Num() < 3) return false;

    TestEqual(TEXT("Element 0 OutputName"), Expr->AdditionalOutputs[0].OutputName, FName(TEXT("Alpha")));
    TestEqual(TEXT("Element 0 OutputType"),
        static_cast<int32>(Expr->AdditionalOutputs[0].OutputType.GetValue()), 0);

    TestEqual(TEXT("Element 1 OutputName"), Expr->AdditionalOutputs[1].OutputName, FName(TEXT("RGB")));
    TestEqual(TEXT("Element 1 OutputType"),
        static_cast<int32>(Expr->AdditionalOutputs[1].OutputType.GetValue()), 2);

    TestEqual(TEXT("Element 2 OutputName"), Expr->AdditionalOutputs[2].OutputName, FName(TEXT("RGBA")));
    TestEqual(TEXT("Element 2 OutputType"),
        static_cast<int32>(Expr->AdditionalOutputs[2].OutputType.GetValue()), 3);

    return true;
}
