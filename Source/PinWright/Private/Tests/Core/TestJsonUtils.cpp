// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for JsonUtils: ExtractVectorField, ExtractRotatorField, GetJsonStringField
#include "Misc/AutomationTest.h"
#include "PinWrightHelpers.h"
#include "Compat/JsonKeyCompat.h"
#include "Handlers/Image/ImageOps.h"
#include "Utils/JsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// ExtractVectorField
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldValidObjTest,
    "PinWright.core.json.extract_vector_field.ValidObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldValidObjTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>();
    VecObj->SetNumberField(TEXT("x"), 1.0);
    VecObj->SetNumberField(TEXT("y"), 2.0);
    VecObj->SetNumberField(TEXT("z"), 3.0);
    Source->SetObjectField(TEXT("pos"), VecObj);

    FVector Result = ExtractVectorField(Source, TEXT("pos"), FVector::ZeroVector);
    TestEqual(TEXT("X is 1"), (double)Result.X, 1.0);
    TestEqual(TEXT("Y is 2"), (double)Result.Y, 2.0);
    TestEqual(TEXT("Z is 3"), (double)Result.Z, 3.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldValidArrayTest,
    "PinWright.core.json.extract_vector_field.ValidArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldValidArrayTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(4.0));
    Arr.Add(MakeShared<FJsonValueNumber>(5.0));
    Arr.Add(MakeShared<FJsonValueNumber>(6.0));
    Source->SetArrayField(TEXT("pos"), Arr);

    FVector Result = ExtractVectorField(Source, TEXT("pos"), FVector::ZeroVector);
    TestEqual(TEXT("X is 4"), (double)Result.X, 4.0);
    TestEqual(TEXT("Y is 5"), (double)Result.Y, 5.0);
    TestEqual(TEXT("Z is 6"), (double)Result.Z, 6.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldPartialTest,
    "PinWright.core.json.extract_vector_field.PartialObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldPartialTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>();
    VecObj->SetNumberField(TEXT("x"), 10.0);
    // y and z missing -- should use default
    Source->SetObjectField(TEXT("pos"), VecObj);

    FVector Default(0.0, 99.0, 99.0);
    FVector Result = ExtractVectorField(Source, TEXT("pos"), Default);
    TestEqual(TEXT("X is 10"), (double)Result.X, 10.0);
    TestEqual(TEXT("Y is default 99"), (double)Result.Y, 99.0);
    TestEqual(TEXT("Z is default 99"), (double)Result.Z, 99.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldMissingTest,
    "PinWright.core.json.extract_vector_field.Missing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldMissingTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    FVector Default(7.0, 8.0, 9.0);
    FVector Result = ExtractVectorField(Source, TEXT("nonexistent"), Default);
    TestEqual(TEXT("X is default"), (double)Result.X, 7.0);
    TestEqual(TEXT("Y is default"), (double)Result.Y, 8.0);
    TestEqual(TEXT("Z is default"), (double)Result.Z, 9.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldNullSourceTest,
    "PinWright.core.json.extract_vector_field.NullSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldNullSourceTest::RunTest(const FString& Parameters)
{
    FVector Default(1.0, 2.0, 3.0);
    FVector Result = ExtractVectorField(nullptr, TEXT("pos"), Default);
    TestEqual(TEXT("X is default"), (double)Result.X, 1.0);
    TestEqual(TEXT("Y is default"), (double)Result.Y, 2.0);
    TestEqual(TEXT("Z is default"), (double)Result.Z, 3.0);
    return true;
}

// ============================================================================
// ExtractRotatorField
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractRotatorFieldValidObjTest,
    "PinWright.core.json.extract_rotator_field.ValidObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractRotatorFieldValidObjTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
    RotObj->SetNumberField(TEXT("pitch"), 15.0);
    RotObj->SetNumberField(TEXT("yaw"), 90.0);
    RotObj->SetNumberField(TEXT("roll"), 45.0);
    Source->SetObjectField(TEXT("rot"), RotObj);

    FRotator Result = ExtractRotatorField(Source, TEXT("rot"), FRotator::ZeroRotator);
    TestEqual(TEXT("Pitch is 15"), (double)Result.Pitch, 15.0);
    TestEqual(TEXT("Yaw is 90"), (double)Result.Yaw, 90.0);
    TestEqual(TEXT("Roll is 45"), (double)Result.Roll, 45.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractRotatorFieldValidArrayTest,
    "PinWright.core.json.extract_rotator_field.ValidArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractRotatorFieldValidArrayTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(30.0));
    Arr.Add(MakeShared<FJsonValueNumber>(60.0));
    Arr.Add(MakeShared<FJsonValueNumber>(120.0));
    Source->SetArrayField(TEXT("rot"), Arr);

    FRotator Result = ExtractRotatorField(Source, TEXT("rot"), FRotator::ZeroRotator);
    TestEqual(TEXT("Pitch is 30"), (double)Result.Pitch, 30.0);
    TestEqual(TEXT("Yaw is 60"), (double)Result.Yaw, 60.0);
    TestEqual(TEXT("Roll is 120"), (double)Result.Roll, 120.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractRotatorFieldPartialTest,
    "PinWright.core.json.extract_rotator_field.PartialObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractRotatorFieldPartialTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
    RotObj->SetNumberField(TEXT("pitch"), 10.0);
    Source->SetObjectField(TEXT("rot"), RotObj);

    FRotator Default(0.0, 77.0, 88.0);
    FRotator Result = ExtractRotatorField(Source, TEXT("rot"), Default);
    TestEqual(TEXT("Pitch is 10"), (double)Result.Pitch, 10.0);
    TestEqual(TEXT("Yaw is default"), (double)Result.Yaw, 77.0);
    TestEqual(TEXT("Roll is default"), (double)Result.Roll, 88.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractRotatorFieldMissingTest,
    "PinWright.core.json.extract_rotator_field.Missing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractRotatorFieldMissingTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    FRotator Default(11.0, 22.0, 33.0);
    FRotator Result = ExtractRotatorField(Source, TEXT("nonexistent"), Default);
    TestEqual(TEXT("Pitch is default"), (double)Result.Pitch, 11.0);
    TestEqual(TEXT("Yaw is default"), (double)Result.Yaw, 22.0);
    TestEqual(TEXT("Roll is default"), (double)Result.Roll, 33.0);
    return true;
}

// ============================================================================
// GetJsonStringField
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetJsonStringFieldExistingTest,
    "PinWright.core.json.get_json_string_field.Existing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetJsonStringFieldExistingTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), TEXT("MyActor"));

    FString Result = GetJsonStringField(Obj, TEXT("name"));
    TestEqual(TEXT("Returns existing value"), Result, TEXT("MyActor"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetJsonStringFieldMissingTest,
    "PinWright.core.json.get_json_string_field.Missing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetJsonStringFieldMissingTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    FString Result = GetJsonStringField(Obj, TEXT("missing"));
    TestEqual(TEXT("Returns empty default"), Result, TEXT(""));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetJsonStringFieldDefaultTest,
    "PinWright.core.json.get_json_string_field.CustomDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetJsonStringFieldDefaultTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    FString Result = GetJsonStringField(Obj, TEXT("missing"), TEXT("fallback"));
    TestEqual(TEXT("Returns custom default"), Result, TEXT("fallback"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetJsonStringFieldNullObjTest,
    "PinWright.core.json.get_json_string_field.NullObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetJsonStringFieldNullObjTest::RunTest(const FString& Parameters)
{
    FString Result = GetJsonStringField(nullptr, TEXT("field"), TEXT("safe"));
    TestEqual(TEXT("Returns default for null obj"), Result, TEXT("safe"));
    return true;
}

// ============================================================================
// ExtractVectorField.ShortArray — array with <3 elements returns default
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldShortArrayTest,
    "PinWright.core.json.extract_vector_field.ShortArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldShortArrayTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(1.0));
    Arr.Add(MakeShared<FJsonValueNumber>(2.0));
    // Only 2 elements — fewer than 3
    Source->SetArrayField(TEXT("pos"), Arr);

    FVector Default(7.0, 8.0, 9.0);
    FVector Result = ExtractVectorField(Source, TEXT("pos"), Default);
    TestEqual(TEXT("X is default"), (double)Result.X, 7.0);
    TestEqual(TEXT("Y is default"), (double)Result.Y, 8.0);
    TestEqual(TEXT("Z is default"), (double)Result.Z, 9.0);
    return true;
}

// ============================================================================
// ExtractRotatorField.NullSource — returns default
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractRotatorFieldNullSourceTest,
    "PinWright.core.json.extract_rotator_field.NullSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractRotatorFieldNullSourceTest::RunTest(const FString& Parameters)
{
    FRotator Default(11.0, 22.0, 33.0);
    FRotator Result = ExtractRotatorField(nullptr, TEXT("rot"), Default);
    TestEqual(TEXT("Pitch is default"), (double)Result.Pitch, 11.0);
    TestEqual(TEXT("Yaw is default"), (double)Result.Yaw, 22.0);
    TestEqual(TEXT("Roll is default"), (double)Result.Roll, 33.0);
    return true;
}

// ============================================================================
// ExtractRotatorField.ShortArray — array with <3 elements returns default
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractRotatorFieldShortArrayTest,
    "PinWright.core.json.extract_rotator_field.ShortArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractRotatorFieldShortArrayTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(10.0));
    // Only 1 element
    Source->SetArrayField(TEXT("rot"), Arr);

    FRotator Default(44.0, 55.0, 66.0);
    FRotator Result = ExtractRotatorField(Source, TEXT("rot"), Default);
    TestEqual(TEXT("Pitch is default"), (double)Result.Pitch, 44.0);
    TestEqual(TEXT("Yaw is default"), (double)Result.Yaw, 55.0);
    TestEqual(TEXT("Roll is default"), (double)Result.Roll, 66.0);
    return true;
}

// ============================================================================
// ExtractVectorField far-from-origin precision — FVector is double throughout
// UE5, so a coordinate that survives a double round trip must survive this one.
// Both values below are exact in binary64 and inexact in binary32: 1000003000
// (10,000.03 km) is the nearest float to 1000003008, and 1e12 / 1e12+3000
// (10,000,000 km) collapse onto the same float 999999995904. Tolerance is 0.0
// on purpose — the default KINDA_SMALL_NUMBER would still catch these, but any
// looser tolerance test would pass against a float32 narrowing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldFarFromOriginObjectTest,
    "PinWright.core.json.extract_vector_field.FarFromOriginObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldFarFromOriginObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>();
    VecObj->SetNumberField(TEXT("x"), 1000003000.0);
    VecObj->SetNumberField(TEXT("y"), -1000003000.0);
    VecObj->SetNumberField(TEXT("z"), 1000000003000.0);
    Source->SetObjectField(TEXT("pos"), VecObj);

    FVector Result = ExtractVectorField(Source, TEXT("pos"), FVector::ZeroVector);
    TestEqual(TEXT("X round-trips exactly"), (double)Result.X, 1000003000.0, 0.0);
    TestEqual(TEXT("Y round-trips exactly"), (double)Result.Y, -1000003000.0, 0.0);
    TestEqual(TEXT("Z round-trips exactly"), (double)Result.Z, 1000000003000.0, 0.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtractVectorFieldFarFromOriginSpanTest,
    "PinWright.core.json.extract_vector_field.FarFromOriginSpan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExtractVectorFieldFarFromOriginSpanTest::RunTest(const FString& Parameters)
{
    // Array form, and the case that turns a precision loss into a wrong answer:
    // a 3000 cm region collapses to a zero-width one and callers are told their
    // max does not exceed their min.
    TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> MinArr;
    MinArr.Add(MakeShared<FJsonValueNumber>(1000000000000.0));
    MinArr.Add(MakeShared<FJsonValueNumber>(1000000000000.0));
    MinArr.Add(MakeShared<FJsonValueNumber>(1000000000000.0));
    Source->SetArrayField(TEXT("min"), MinArr);

    TArray<TSharedPtr<FJsonValue>> MaxArr;
    MaxArr.Add(MakeShared<FJsonValueNumber>(1000000003000.0));
    MaxArr.Add(MakeShared<FJsonValueNumber>(1000000003000.0));
    MaxArr.Add(MakeShared<FJsonValueNumber>(1000000003000.0));
    Source->SetArrayField(TEXT("max"), MaxArr);

    const FVector Min = ExtractVectorField(Source, TEXT("min"), FVector::ZeroVector);
    const FVector Max = ExtractVectorField(Source, TEXT("max"), FVector::ZeroVector);

    TestEqual(TEXT("Span X is 3000"), (double)(Max.X - Min.X), 3000.0, 0.0);
    TestEqual(TEXT("Span Y is 3000"), (double)(Max.Y - Min.Y), 3000.0, 0.0);
    TestEqual(TEXT("Span Z is 3000"), (double)(Max.Z - Min.Z), 3000.0, 0.0);
    TestTrue(TEXT("Max exceeds Min on every axis"),
        Max.X > Min.X && Max.Y > Min.Y && Max.Z > Min.Z);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRejectUnknownJsonKeysPoliciesTest,
    "PinWright.core.json.reject_unknown_keys.PoliciesPreserveOrdering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRejectUnknownJsonKeysPoliciesTest::RunTest(const FString& Parameters)
{
    const TArray<FString> Allowed = { TEXT("known") };
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("zeta"), 1.0);
    Object->SetNumberField(TEXT("known"), 2.0);
    Object->SetNumberField(TEXT("alpha"), 3.0);

    TArray<FString> Unknown;
    TestFalse(TEXT("first mode rejects an unknown key"),
        RejectUnknownKeys(Object, Allowed, Unknown, ERejectUnknownKeysMode::First));
    TestEqual(TEXT("first mode stops after one key"), Unknown.Num(), 1);
    FString FirstEncounteredUnknown;
    for (const auto& Pair : Object->Values)
    {
        const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
        if (!Allowed.ContainsByPredicate([&Key](const FString& Candidate)
            { return Candidate.Equals(Key, ESearchCase::CaseSensitive); }))
        {
            FirstEncounteredUnknown = Key;
            break;
        }
    }
    if (Unknown.Num() == 1)
    {
        TestEqual(TEXT("first mode returns the first encountered unknown key"),
            Unknown[0], FirstEncounteredUnknown);
    }

    TestFalse(TEXT("all-sorted mode rejects unknown keys"),
        RejectUnknownKeys(Object, Allowed, Unknown, ERejectUnknownKeysMode::AllSorted));
    TestEqual(TEXT("all-sorted mode returns every unknown key"), Unknown.Num(), 2);
    if (Unknown.Num() == 2)
    {
        TestEqual(TEXT("all-sorted mode sorts the first key"), Unknown[0], TEXT("alpha"));
        TestEqual(TEXT("all-sorted mode sorts the second key"), Unknown[1], TEXT("zeta"));
    }

    TSharedPtr<FJsonObject> DifferentlyCased = MakeShared<FJsonObject>();
    DifferentlyCased->SetBoolField(TEXT("Known"), true);
    TestFalse(TEXT("shared helper defaults to case-sensitive matching"),
        RejectUnknownKeys(DifferentlyCased, Allowed, Unknown));
    TestTrue(TEXT("callers can explicitly opt into case-insensitive matching"),
        RejectUnknownKeys(DifferentlyCased, Allowed, Unknown,
            ERejectUnknownKeysMode::First, ESearchCase::IgnoreCase));

    FString ImageError;
    TestFalse(TEXT("image adapter keeps its case-sensitive key contract"),
        PinWrightImage::RejectUnknownKeys(DifferentlyCased, Allowed, TEXT("image"), ImageError));
    TestTrue(TEXT("image adapter reports the differently-cased key"), ImageError.Contains(TEXT("Known")));

    Unknown.Add(TEXT("stale"));
    TestTrue(TEXT("a null object is accepted"), RejectUnknownKeys(nullptr, Allowed, Unknown));
    TestEqual(TEXT("a null object clears stale output"), Unknown.Num(), 0);
    return true;
}
