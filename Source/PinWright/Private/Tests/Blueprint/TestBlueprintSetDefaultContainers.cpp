// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for E-set-default-rejects-container-typed-properties: blueprint.set_default declared
// `value` as "string", so the dispatcher refused every JSON array/object before the handler ran,
// and the shared importer had no TSet/TMap branch at all ("Unsupported property type for JSON
// assignment"). A map variable that blueprint.add_variable happily created could not be given a
// default. These tests drive the real handler on a transient Actor Blueprint.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetNavigation.h"
#include "Handlers/ParamTypeCheck.h"
#include "UObject/UnrealType.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestUtils.h"
#include "WidgetBlueprint.h"

namespace SetDefaultContainerTestHelpers
{
    const TCHAR* const MapVar = TEXT("DecalSizes");   // map<enum<EPhysicalSurface>,float>
    const TCHAR* const ArrayVar = TEXT("TagList");    // array<name>
    const TCHAR* const SetVar = TEXT("IdSet");        // set<int>

    bool AddVariable(FAutomationTestBase& Test, UBlueprint* BP, const TCHAR* Name, const TCHAR* Type)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BP->GetPathName());
        Payload->SetStringField(TEXT("variableName"), Name);
        Payload->SetStringField(TEXT("variableType"), Type);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), Payload, Capture);
        return Test.TestTrue(*FString::Printf(TEXT("precondition: add_variable %s %s"), Type, Name), Capture.bSuccess);
    }

    FTestResponseCapture SetDefault(UBlueprint* BP, const TCHAR* Name, const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), BP->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), Name);
        Payload->SetField(TEXT("value"), Value);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.set_default"), Payload, Capture);
        return Capture;
    }

    TSharedPtr<FJsonValue> SurfaceMap(const TCHAR* FirstKey, double First, const TCHAR* SecondKey, double Second)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(FirstKey, First);
        Obj->SetNumberField(SecondKey, Second);
        return MakeShared<FJsonValueObject>(Obj);
    }

    // The CDO's map as {enum value -> float}; EPhysicalSurface::SurfaceTypeN has value N.
    // Bound by GetMaxIndex (sparse storage).
    TMap<int64, double> ReadSurfaceMap(UBlueprint* BP)
    {
        TMap<int64, double> Out;
        UObject* CDO = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
        FMapProperty* MP = CDO ? FindFProperty<FMapProperty>(CDO->GetClass(), MapVar) : nullptr;
        FNumericProperty* ValueProp = MP ? CastField<FNumericProperty>(MP->ValueProp) : nullptr;
        if (!ValueProp)
        {
            return Out;
        }
        FScriptMapHelper Helper(MP, MP->ContainerPtrToValuePtr<void>(CDO));
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i))
            {
                continue;
            }
            int64 Key = *Helper.GetKeyPtr(i);
            if (FEnumProperty* EnumKey = CastField<FEnumProperty>(MP->KeyProp))
            {
                Key = EnumKey->GetUnderlyingProperty()->GetSignedIntPropertyValue(Helper.GetKeyPtr(i));
            }
            Out.Add(Key, ValueProp->GetFloatingPointPropertyValue(Helper.GetValuePtr(i)));
        }
        return Out;
    }

    UBlueprint* MakeContainerBP(FAutomationTestBase& Test)
    {
        UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("SetDefaultContainers"));
        if (!Test.TestNotNull(TEXT("Blueprint created"), BP))
        {
            return nullptr;
        }
        const bool bOk = AddVariable(Test, BP, MapVar, TEXT("map<enum<EPhysicalSurface>,float>"))
            && AddVariable(Test, BP, ArrayVar, TEXT("array<name>"))
            && AddVariable(Test, BP, SetVar, TEXT("set<int>"));
        return bOk ? BP : nullptr;
    }
}

// The declared type is what real callers hit: a "string" slot is refused for arrays and objects
// by the dispatcher's declared-type gate before the handler runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetDefaultValueSlotTakesContainersTest,
    "PinWright.blueprint.set_default.ValueSlotAdmitsJsonContainers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetDefaultValueSlotTakesContainersTest::RunTest(const FString& Parameters)
{
    const FParamSpec* Spec = ParamSpecTestHelpers::FindParamSpec(TEXT("blueprint.set_default"), TEXT("value"));
    if (!TestNotNull(TEXT("blueprint.set_default declares value"), Spec))
    {
        return true;
    }
    const TSharedPtr<FJsonValue> Arr = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
    const TSharedPtr<FJsonValue> Obj = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
    TestTrue(TEXT("declared type admits a JSON array"),
        PinWrightParamTypes::PinWrightCheckDeclaredType(Spec->Type, Arr) == PinWrightParamTypes::EDeclaredTypeVerdict::Accepted);
    TestTrue(TEXT("declared type admits a JSON object"),
        PinWrightParamTypes::PinWrightCheckDeclaredType(Spec->Type, Obj) == PinWrightParamTypes::EDeclaredTypeVerdict::Accepted);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetDefaultContainersAppliedTest,
    "PinWright.blueprint.set_default.MapArraySetDefaultsApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetDefaultContainersAppliedTest::RunTest(const FString& Parameters)
{
    using namespace SetDefaultContainerTestHelpers;
    UBlueprint* BP = MakeContainerBP(*this);
    if (!BP)
    {
        return true;
    }

    // Map: native JSON object keyed by enumerator name (the ticket's exact shape).
    const FTestResponseCapture MapCapture = SetDefault(BP, MapVar, SurfaceMap(TEXT("SurfaceType1"), 14.0, TEXT("SurfaceType6"), 30.0));
    TestTrue(*FString::Printf(TEXT("map default accepted (%s: %s)"), *MapCapture.ErrorCode, *MapCapture.Message), MapCapture.bSuccess);
    const TMap<int64, double> Map = ReadSurfaceMap(BP);
    TestEqual(TEXT("CDO map has two entries"), Map.Num(), 2);
    const double* Type1 = Map.Find(1);
    const double* Type6 = Map.Find(6);
    TestTrue(TEXT("SurfaceType1 -> 14"), Type1 && FMath::IsNearlyEqual(*Type1, 14.0));
    TestTrue(TEXT("SurfaceType6 -> 30"), Type6 && FMath::IsNearlyEqual(*Type6, 30.0));

    // Array: native JSON array.
    TArray<TSharedPtr<FJsonValue>> Tags;
    Tags.Add(MakeShared<FJsonValueString>(TEXT("Team0")));
    Tags.Add(MakeShared<FJsonValueString>(TEXT("Team1")));
    const FTestResponseCapture ArrayCapture = SetDefault(BP, ArrayVar, MakeShared<FJsonValueArray>(Tags));
    TestTrue(*FString::Printf(TEXT("array default accepted (%s: %s)"), *ArrayCapture.ErrorCode, *ArrayCapture.Message), ArrayCapture.bSuccess);

    // Set: JSON text, the form a caller of the old "string" slot sends.
    const FTestResponseCapture SetCapture = SetDefault(BP, SetVar, MakeShared<FJsonValueString>(TEXT("[3, 5, 8]")));
    TestTrue(*FString::Printf(TEXT("set default accepted from JSON text (%s: %s)"), *SetCapture.ErrorCode, *SetCapture.Message), SetCapture.bSuccess);

    UObject* CDO = BP->GeneratedClass->GetDefaultObject();
    if (FArrayProperty* AP = FindFProperty<FArrayProperty>(CDO->GetClass(), ArrayVar))
    {
        FScriptArrayHelper Helper(AP, AP->ContainerPtrToValuePtr<void>(CDO));
        TestEqual(TEXT("CDO array has two elements"), Helper.Num(), 2);
        if (Helper.Num() == 2)
        {
            TestEqual(TEXT("array[1]"), reinterpret_cast<FName*>(Helper.GetRawPtr(1))->ToString(), FString(TEXT("Team1")));
        }
    }
    else
    {
        AddError(TEXT("array variable missing on CDO"));
    }
    if (FSetProperty* SP = FindFProperty<FSetProperty>(CDO->GetClass(), SetVar))
    {
        FScriptSetHelper Helper(SP, SP->ContainerPtrToValuePtr<void>(CDO));
        TestEqual(TEXT("CDO set has three elements"), Helper.Num(), 3);
    }
    else
    {
        AddError(TEXT("set variable missing on CDO"));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetDefaultContainerFailureLeavesValueTest,
    "PinWright.blueprint.set_default.BadMapKeyRefusedWithoutWriting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetDefaultContainerFailureLeavesValueTest::RunTest(const FString& Parameters)
{
    using namespace SetDefaultContainerTestHelpers;
    UBlueprint* BP = MakeContainerBP(*this);
    if (!BP)
    {
        return true;
    }
    const FTestResponseCapture Seed = SetDefault(BP, MapVar, SurfaceMap(TEXT("SurfaceType1"), 14.0, TEXT("SurfaceType2"), 20.0));
    if (!TestTrue(*FString::Printf(TEXT("precondition: seed map default (%s: %s)"), *Seed.ErrorCode, *Seed.Message), Seed.bSuccess))
    {
        return true;
    }

    // One good entry and one key that is no EPhysicalSurface enumerator: the whole write is refused.
    const FTestResponseCapture Bad = SetDefault(BP, MapVar, SurfaceMap(TEXT("SurfaceType3"), 99.0, TEXT("NotASurface"), 1.0));
    TestFalse(TEXT("bad key is refused"), Bad.bSuccess);
    TestEqual(TEXT("error code"), Bad.ErrorCode, FString(TEXT("CONVERSION_FAILED")));
    TestTrue(*FString::Printf(TEXT("error names the bad key and says nothing was written: %s"), *Bad.Message),
        Bad.Message.Contains(TEXT("NotASurface")) && Bad.Message.Contains(TEXT("was not modified")));

    const TMap<int64, double> Map = ReadSurfaceMap(BP);
    TestEqual(TEXT("CDO map still holds the seeded two entries"), Map.Num(), 2);
    TestFalse(TEXT("the good entry of the refused write did not land"), Map.Contains(3));
    return true;
}

// An Instanced object property given as its export brace text is rebuilt as a subobject outered to
// the container, so it must skip the scratch pre-check (a scratch base is no UObject). Reverting the
// IsInstancedSubobjectText skip in blueprint.set_default crashes or fails this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetDefaultInstancedNavigationTest,
    "PinWright.blueprint.set_default.InstancedNavigationBraceTextApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetDefaultInstancedNavigationTest::RunTest(const FString& Parameters)
{
    using namespace SetDefaultContainerTestHelpers;
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UWidgetBlueprint::StaticClass(), TEXT("SetDefaultNavigationWBP")),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Widget Blueprint created"), BP) || !TestNotNull(TEXT("generated class"), BP->GeneratedClass.Get()))
    {
        return true;
    }
    const UUserWidget* Before = Cast<UUserWidget>(BP->GeneratedClass->GetDefaultObject());
    if (!TestNotNull(TEXT("precondition: CDO is a UUserWidget"), Before)
        || !TestNull(TEXT("precondition: CDO has no Navigation yet"), Before->Navigation.Get()))
    {
        return true;
    }

    // Rule=Stop: FWidgetNavigationData defaults to Escape, so only a real import changes it.
    const FTestResponseCapture Capture = SetDefault(BP, TEXT("Navigation"),
        MakeShared<FJsonValueString>(TEXT("{Down={Rule=Stop},_kind=/Script/UMG.WidgetNavigation}")));
    TestTrue(*FString::Printf(TEXT("Navigation brace text accepted (%s: %s)"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);

    UUserWidget* CDO = Cast<UUserWidget>(BP->GeneratedClass->GetDefaultObject());
    UWidgetNavigation* Nav = CDO ? CDO->Navigation.Get() : nullptr;
    if (!TestNotNull(TEXT("CDO has a Navigation subobject"), Nav))
    {
        return true;
    }
    TestTrue(TEXT("Navigation is outered to the CDO"), Nav->GetOuter() == CDO);
    TestTrue(TEXT("a CDO's subobject is a default subobject"), Nav->HasAnyFlags(RF_DefaultSubObject));
    TestTrue(TEXT("Down rule imported"), Nav->Down.Rule == EUINavigationRule::Stop);
    return true;
}
