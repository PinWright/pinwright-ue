// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Animation/AnimSequence.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpAnimSequenceSuppressesTransientControllerTest,
    "PinWright.utils.asset_dump_anim_sequence.SuppressesTransientController",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpAnimSequenceSuppressesTransientControllerTest::RunTest(const FString& Parameters)
{
    UAnimSequence* AnimSequence = NewObject<UAnimSequence>(GetTransientPackage());
    TestNotNull(TEXT("Transient AnimSequence created"), AnimSequence);
    if (!AnimSequence) return false;

    AnimSequence->GetController();

    FProperty* ControllerProp = UAnimSequence::StaticClass()->FindPropertyByName(TEXT("Controller"));
    TestNotNull(TEXT("UAnimSequence has Controller property"), ControllerProp);
    if (!ControllerProp) return false;

    TSharedPtr<FJsonValue> RawControllerValue = ExportPropertyToJsonValue(AnimSequence, ControllerProp);
    TestNotNull(TEXT("Controller exports through the low-level property serializer"), RawControllerValue.Get());
    if (!RawControllerValue) return false;
    TestEqual(TEXT("Controller low-level export is an object path"), RawControllerValue->Type, EJson::String);
    if (RawControllerValue->Type == EJson::String)
    {
        TestTrue(TEXT("Controller low-level export is session-scoped /Engine/Transient data"),
            RawControllerValue->AsString().StartsWith(TEXT("/Engine/Transient")));
    }

    TSharedPtr<FJsonObject> Result = BuildClassPropertyJson(
        AnimSequence,
        UAnimSequence::StaticClass()->GetDefaultObject());
    TestNotNull(TEXT("BuildClassPropertyJson returned non-null"), Result.Get());
    if (!Result) return false;

    // Counterfactual: if suppression in PropertyUtils.cpp is reverted, this
    // assertion fails because UAnimSequenceBase constructs a transient Controller
    // object and BuildClassPropertyJson emits it as /Engine/Transient.AnimSequencerController_<N>.
    TestFalse(TEXT("Controller transient package reference is suppressed from properties.json"),
        Result->HasField(TEXT("Controller")));

    return true;
}
