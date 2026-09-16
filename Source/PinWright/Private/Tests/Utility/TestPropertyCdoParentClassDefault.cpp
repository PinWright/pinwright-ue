// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPropertyCdoParentClassDefault.cpp
// Regression test for B-property-reset-cdo-uses-self-not-archetype.
//
// When the target object is a Blueprint generated-class CDO, the per-property
// default for an *inherited, locally-unauthored* property must come from the
// object's archetype (the parent-class CDO via UObject::GetArchetype() ->
// UBlueprintGeneratedClass::GetArchetypeForCDO()), mirroring the editor's
// per-property reset arrow. Before the fix, ResolveDefaultSourceObject fell
// straight through to ResolveClassDefaultObject() -> GetClass()->GetDefaultObject(),
// which for a CDO IS the CDO itself, so:
//   * property.get/list reported defaultSource:"class_cdo" with defaultValue ==
//     the CDO's own (mutated) value, isOverridden:false;
//   * property.reset copied the CDO value onto itself (a guaranteed no-op) with
//     wasOverridden:false.
//
// This test builds a parent BP (subclass of AActor) with an int member variable
// authored to a known value on the parent CDO, a child BP that subclasses it,
// then mutates the inherited value on the *child* CDO and drives the production
// property.get / property.reset / property.list handlers.
//
// Counterfactual: revert ResolveBlueprintCdoArchetype / the parent_class_cdo
// branch in ResolveDefaultSourceObject and the default source collapses back to
// "class_cdo" with the mutated value as the default, reset becomes a no-op, and
// every assertion below fails.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"
#include "Tests/TestUtils.h"
#include "Tests/Utility/PropertyListTestHelpers.h"

namespace
{
    bool InvokeCdoPropertyHandler(
        FAutomationTestBase& Test,
        const FString& MethodName,
        const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture)
    {
        Test.TestTrue(
            FString::Printf(TEXT("%s handler found"), *MethodName),
            InvokeHandlerWithCapture(MethodName, Payload, Capture));
        Test.TestTrue(
            FString::Printf(TEXT("%s succeeded"), *MethodName),
            Capture.bSuccess);
        Test.TestTrue(
            FString::Printf(TEXT("%s returned payload"), *MethodName),
            Capture.Result.IsValid());
        return Capture.bSuccess && Capture.Result.IsValid();
    }

    FName MakeUniqueCdoBlueprintName(const TCHAR* Prefix)
    {
        return FName(*FString::Printf(
            TEXT("%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }

    // Sets the named int property on the object's class default via reflection.
    bool SetIntCdoValue(UObject* CDO, const TCHAR* PropertyName, int32 Value)
    {
        if (!CDO)
        {
            return false;
        }
        FIntProperty* IntProp =
            CastField<FIntProperty>(CDO->GetClass()->FindPropertyByName(PropertyName));
        if (!IntProp)
        {
            return false;
        }
        IntProp->SetPropertyValue_InContainer(CDO, Value);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyCdoParentClassDefaultTest,
    "PinWright.property.cdo_parent_class_default.InheritedCdoFieldResolvesParentClassCdo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyCdoParentClassDefaultTest::RunTest(const FString& Parameters)
{
    // 1. Parent BP (subclass of AActor) with an int member 'InheritedScalar'
    //    authored to 3 on the parent CDO.
    const FName ParentName = MakeUniqueCdoBlueprintName(TEXT("BP_CdoParentDefault_Parent"));
    UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        ParentName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("parent blueprint created"), ParentBP);
    if (!ParentBP)
    {
        return false;
    }

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const bool bVarAdded =
        FBlueprintEditorUtils::AddMemberVariable(ParentBP, TEXT("InheritedScalar"), IntType);
    TestTrue(TEXT("InheritedScalar member variable added to parent"), bVarAdded);
    if (!bVarAdded)
    {
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(ParentBP);
    TestNotNull(TEXT("parent generated class after compile"), ParentBP->GeneratedClass.Get());
    if (!ParentBP->GeneratedClass)
    {
        return false;
    }

    UObject* ParentCDO = ParentBP->GeneratedClass->GetDefaultObject();
    TestNotNull(TEXT("parent CDO available"), ParentCDO);
    TestTrue(TEXT("authored InheritedScalar=3 on parent CDO"),
        SetIntCdoValue(ParentCDO, TEXT("InheritedScalar"), 3));

    // 2. Child BP subclassing the parent. The child CDO inherits InheritedScalar.
    const FName ChildName = MakeUniqueCdoBlueprintName(TEXT("BP_CdoParentDefault_Child"));
    UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
        ParentBP->GeneratedClass,
        GetTransientPackage(),
        ChildName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("child blueprint created"), ChildBP);
    if (!ChildBP)
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(ChildBP);
    TestNotNull(TEXT("child generated class after compile"), ChildBP->GeneratedClass.Get());
    if (!ChildBP->GeneratedClass)
    {
        return false;
    }

    UObject* ChildCDO = ChildBP->GeneratedClass->GetDefaultObject();
    TestNotNull(TEXT("child CDO available"), ChildCDO);
    if (!ChildCDO)
    {
        return false;
    }
    TestTrue(TEXT("child CDO is a class default object"),
        ChildCDO->HasAnyFlags(RF_ClassDefaultObject));

    // 3. Mutate the inherited value on the *child* CDO (simulates property.set
    //    baking a value into the CDO). The parent CDO still holds 3.
    TestTrue(TEXT("mutated InheritedScalar=9 on child CDO"),
        SetIntCdoValue(ChildCDO, TEXT("InheritedScalar"), 9));

    const FString ChildCdoPath = ChildCDO->GetPathName();

    // 4. property.get: default must come from the parent-class CDO (value 3),
    //    NOT the child CDO's own mutated value (9). Source must distinguish it.
    {
        TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
        GetPayload->SetStringField(TEXT("objectPath"), ChildCdoPath);
        GetPayload->SetStringField(TEXT("propertyName"), TEXT("InheritedScalar"));
        GetPayload->SetBoolField(TEXT("includeDefault"), true);
        GetPayload->SetBoolField(TEXT("includeOverrideState"), true);

        FTestResponseCapture GetCapture;
        if (!InvokeCdoPropertyHandler(*this, TEXT("property.get"), GetPayload, GetCapture))
        {
            return false;
        }

        double CurrentValue = 0.0;
        TestTrue(TEXT("property.get current value present"),
            GetCapture.Result->TryGetNumberField(TEXT("value"), CurrentValue));
        TestEqual(TEXT("property.get current value is the mutated 9"),
            static_cast<int32>(CurrentValue), 9);

        FString DefaultSource;
        TestTrue(TEXT("property.get default source is parent_class_cdo"),
            GetCapture.Result->TryGetStringField(TEXT("defaultSource"), DefaultSource) &&
            DefaultSource == TEXT("parent_class_cdo"));

        double DefaultValue = 0.0;
        TestTrue(TEXT("property.get default value present"),
            GetCapture.Result->TryGetNumberField(TEXT("defaultValue"), DefaultValue));
        TestEqual(TEXT("property.get default value comes from parent CDO (3)"),
            static_cast<int32>(DefaultValue), 3);

        bool bIsOverridden = false;
        TestTrue(TEXT("property.get isOverridden present"),
            GetCapture.Result->TryGetBoolField(TEXT("isOverridden"), bIsOverridden));
        TestTrue(TEXT("property.get reports overridden (9 != 3)"), bIsOverridden);
    }

    // 5. property.reset: must restore the parent default (3) and report
    //    wasOverridden:true with a differing oldValue/defaultValue.
    {
        TSharedPtr<FJsonObject> ResetPayload = MakeShared<FJsonObject>();
        ResetPayload->SetStringField(TEXT("objectPath"), ChildCdoPath);
        ResetPayload->SetStringField(TEXT("propertyName"), TEXT("InheritedScalar"));
        ResetPayload->SetBoolField(TEXT("markDirty"), false);

        FTestResponseCapture ResetCapture;
        if (!InvokeCdoPropertyHandler(*this, TEXT("property.reset"), ResetPayload, ResetCapture))
        {
            return false;
        }

        FString DefaultSource;
        TestTrue(TEXT("property.reset default source is parent_class_cdo"),
            ResetCapture.Result->TryGetStringField(TEXT("defaultSource"), DefaultSource) &&
            DefaultSource == TEXT("parent_class_cdo"));

        double OldValue = 0.0;
        TestTrue(TEXT("property.reset oldValue present"),
            ResetCapture.Result->TryGetNumberField(TEXT("oldValue"), OldValue));
        TestEqual(TEXT("property.reset oldValue is the mutated 9"),
            static_cast<int32>(OldValue), 9);

        double DefaultValue = 0.0;
        TestTrue(TEXT("property.reset defaultValue present"),
            ResetCapture.Result->TryGetNumberField(TEXT("defaultValue"), DefaultValue));
        TestEqual(TEXT("property.reset defaultValue is parent CDO 3"),
            static_cast<int32>(DefaultValue), 3);

        bool bWasOverridden = false;
        TestTrue(TEXT("property.reset wasOverridden present"),
            ResetCapture.Result->TryGetBoolField(TEXT("wasOverridden"), bWasOverridden));
        TestTrue(TEXT("property.reset reports it WAS overridden"), bWasOverridden);

        // The actual CDO value must now equal the parent default.
        FIntProperty* IntProp =
            CastField<FIntProperty>(ChildCDO->GetClass()->FindPropertyByName(TEXT("InheritedScalar")));
        TestNotNull(TEXT("InheritedScalar property resolvable on child CDO"), IntProp);
        if (IntProp)
        {
            TestEqual(TEXT("reset restored child CDO InheritedScalar to parent default (3)"),
                IntProp->GetPropertyValue_InContainer(ChildCDO), 3);
        }
    }

    // 6. property.list after reset: value now matches the parent default, source
    //    is still parent_class_cdo, and isOverridden is false.
    {
        TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
        ListPayload->SetStringField(TEXT("objectPath"), ChildCdoPath);
        ListPayload->SetBoolField(TEXT("includeValues"), true);
        ListPayload->SetBoolField(TEXT("includeDefault"), true);
        ListPayload->SetBoolField(TEXT("includeOverrideState"), true);
        ListPayload->SetStringField(TEXT("nameMatch"), TEXT("InheritedScalar"));

        FTestResponseCapture ListCapture;
        if (!InvokeCdoPropertyHandler(*this, TEXT("property.list"), ListPayload, ListCapture))
        {
            return false;
        }

        TSharedPtr<FJsonObject> Entry =
            PropertyListTestHelpers::FindPropertyEntry(ListCapture.Result, TEXT("InheritedScalar"));
        TestTrue(TEXT("property.list includes InheritedScalar"), Entry.IsValid());
        if (!Entry.IsValid())
        {
            return false;
        }

        FString DefaultSource;
        TestTrue(TEXT("property.list default source is parent_class_cdo"),
            Entry->TryGetStringField(TEXT("defaultSource"), DefaultSource) &&
            DefaultSource == TEXT("parent_class_cdo"));

        double CurrentValue = 0.0;
        TestTrue(TEXT("property.list value present"),
            Entry->TryGetNumberField(TEXT("value"), CurrentValue));
        TestEqual(TEXT("property.list value is the reset 3"),
            static_cast<int32>(CurrentValue), 3);

        double DefaultValue = 0.0;
        TestTrue(TEXT("property.list default value present"),
            Entry->TryGetNumberField(TEXT("defaultValue"), DefaultValue));
        TestEqual(TEXT("property.list default value is parent CDO 3"),
            static_cast<int32>(DefaultValue), 3);

        bool bIsOverridden = true;
        TestTrue(TEXT("property.list isOverridden present after reset"),
            Entry->TryGetBoolField(TEXT("isOverridden"), bIsOverridden));
        TestFalse(TEXT("property.list reports not overridden after reset"), bIsOverridden);
    }

    return true;
}
