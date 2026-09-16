// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Utils/SortedJsonWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"

#include "Components/SceneComponent.h"
#include "Components/ActorComponent.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

// ============================================================================
// AssetDumpJsonShape.PrunedFieldsAbsent
// Schema v2: per-property records omit `flags`, `inherited_from`, and
// `is_overridden_locally` when their value is the implicit default.
// `type` and `value` always remain present.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpJsonShapePrunedFieldsAbsentTest,
    "PinWright.Utils.AssetDumpJsonShape.PrunedFieldsAbsent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpJsonShapePrunedFieldsAbsentTest::RunTest(const FString& Parameters)
{
    // RelativeLocation is declared on USceneComponent. With AssetClass=USceneComponent
    // and ParentContainer pointing at a same-valued sibling, all three pruned fields
    // collapse to their defaults: no flags whitelisted? → flags absent;
    // own-class? → inherited_from absent; same value? → is_overridden_locally absent.
    //
    // RelativeLocation has CPF_Edit, so flags WILL be present in this case.
    // Use a property that actually has no whitelisted flags to exercise pruning.
    FProperty* ComponentVelocityProp =
        USceneComponent::StaticClass()->FindPropertyByName(TEXT("ComponentVelocity"));
    TestNotNull(TEXT("USceneComponent has ComponentVelocity"), ComponentVelocityProp);
    if (!ComponentVelocityProp) return false;

    USceneComponent* Child  = NewObject<USceneComponent>(GetTransientPackage());
    USceneComponent* Parent = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Child"), Child);
    TestNotNull(TEXT("Parent"), Parent);
    if (!Child || !Parent) return false;

    // Same value -> not overridden.
    TSharedPtr<FJsonObject> Result = ExportPropertyToJsonValueWithInheritance(
        Child, Parent, ComponentVelocityProp, USceneComponent::StaticClass());
    TestNotNull(TEXT("Result valid"), Result.Get());
    if (!Result) return false;

    // Always present.
    TestTrue(TEXT("type field is always present"), Result->Values.Contains(TEXT("type")));
    TestTrue(TEXT("value field is always present"), Result->Values.Contains(TEXT("value")));

    // own-class, not-overridden, no-edit-flag -> all three pruned.
    TestFalse(TEXT("inherited_from absent for own-class property"),
        Result->Values.Contains(TEXT("inherited_from")));
    TestFalse(TEXT("is_overridden_locally absent for same-value case"),
        Result->Values.Contains(TEXT("is_overridden_locally")));
    TestFalse(TEXT("flags absent when no whitelisted flags apply"),
        Result->Values.Contains(TEXT("flags")));

    return true;
}

// ============================================================================
// AssetDumpJsonShape.InlinePrimitivesNoBrokenLines
// SerializeSortedJsonObject must place primitive values on the same line as their
// key under TPrettyJsonPrintPolicy. Regression-protects the Chunk 1B inline fix.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpJsonShapeInlinePrimitivesNoBrokenLinesTest,
    "PinWright.Utils.AssetDumpJsonShape.InlinePrimitivesNoBrokenLines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpJsonShapeInlinePrimitivesNoBrokenLinesTest::RunTest(const FString& Parameters)
{
    USceneComponent* CDO = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("CDO created"), CDO);
    if (!CDO) return false;

    TSharedPtr<FJsonObject> ClassJson = BuildClassPropertyJson(CDO, nullptr);
    TestNotNull(TEXT("BuildClassPropertyJson returned non-null"), ClassJson.Get());
    if (!ClassJson) return false;

    const FString Serialized = SortedJsonWriter::SerializeSortedJsonObject(ClassJson);
    TestTrue(TEXT("Serialized output is non-empty"), !Serialized.IsEmpty());

    // Look for any primitive value sitting on its own line below its key:
    //   "key":
    //       <primitive>
    // The fix in Chunk 1B places primitive values on the same line as their key,
    // so this regex must NOT match anywhere in the output.
    const FRegexPattern BrokenLinePattern(
        TEXT("\"\\w+\":\\s*\\n\\s+(null|true|false|-?\\d|\")"));
    FRegexMatcher Matcher(BrokenLinePattern, Serialized);
    const bool bHasBrokenLine = Matcher.FindNext();
    TestFalse(
        TEXT("Primitive values must follow their key on the same line (no key:\\n value)"),
        bHasBrokenLine);

    return true;
}

// ============================================================================
// AssetDumpJsonShape.SoftClassTypeNoTrailingSpace
// FSoftClassProperty::GetCPPType() returns "TSoftClassPtr<X> " with a literal
// trailing space (engine quirk at Runtime/CoreUObject/Private/UObject/
// PropertySoftClassPtr.cpp:64). ExportPropertyToJsonValueWithInheritance must
// trim it before writing the `type` field of properties.json.
//
// Counterfactual: if TypeStr.TrimEndInline() is removed from
// ExportPropertyToJsonValueWithInheritance in PropertyUtils.cpp, this test
// fails because TypeStr equals "TSoftClassPtr<UPlayerInput> " (engine appends
// the trailing space) and the equality + EndsWith(" ") checks fail.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpJsonShapeSoftClassTypeNoTrailingSpaceTest,
    "PinWright.Utils.AssetDumpJsonShape.SoftClassTypeNoTrailingSpace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpJsonShapeSoftClassTypeNoTrailingSpaceTest::RunTest(const FString& Parameters)
{
    // UInputSettings::DefaultPlayerInputClass is declared as
    // TSoftClassPtr<UPlayerInput> (UE 5.6 InputSettings.h:175), exercising the
    // FSoftClassProperty path that emits the trailing-space typename.
    FProperty* Prop = UInputSettings::StaticClass()->FindPropertyByName(
        TEXT("DefaultPlayerInputClass"));
    TestNotNull(TEXT("UInputSettings has DefaultPlayerInputClass"), Prop);
    if (!Prop) return false;

    // Fail-fast on engine drift: if the property kind changes in a future UE
    // version this test no longer exercises FSoftClassProperty::GetCPPType.
    FSoftClassProperty* SoftProp = CastField<FSoftClassProperty>(Prop);
    TestNotNull(TEXT("DefaultPlayerInputClass is FSoftClassProperty (engine drift check)"),
        SoftProp);
    if (!SoftProp) return false;

    UInputSettings* Settings = GetMutableDefault<UInputSettings>();
    TestNotNull(TEXT("UInputSettings CDO"), Settings);
    if (!Settings) return false;

    TSharedPtr<FJsonObject> Result = ExportPropertyToJsonValueWithInheritance(
        Settings, Settings, Prop, UInputSettings::StaticClass());
    TestNotNull(TEXT("Result valid"), Result.Get());
    if (!Result) return false;

    const FString TypeStr = Result->GetStringField(TEXT("type"));
    TestEqual(TEXT("type has no trailing whitespace"),
        TypeStr, FString(TEXT("TSoftClassPtr<UPlayerInput>")));
    TestFalse(TEXT("type does not end with space"),
        TypeStr.EndsWith(TEXT(" ")));

    return true;
}
