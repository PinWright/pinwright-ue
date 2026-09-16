// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Tests/TestUtils.h"
#include "Tests/Utility/PropertyListTestHelpers.h"
#include "Utils/PropertyUtils.h"

namespace
{
    bool InvokePropertyHandler(
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

    bool IsSingleStringArrayField(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        const FString& Expected)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() ||
            !Object->TryGetArrayField(FieldName, Values) ||
            !Values ||
            Values->Num() != 1)
        {
            return false;
        }

        const TSharedPtr<FJsonValue>& OnlyValue = (*Values)[0];
        return OnlyValue.IsValid() &&
            OnlyValue->Type == EJson::String &&
            OnlyValue->AsString() == Expected;
    }

    TSharedPtr<FJsonObject> MakeComponentTagsListPayload(UActorComponent* ComponentTemplate)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), ComponentTemplate->GetPathName());
        Payload->SetBoolField(TEXT("includeAll"), true);
        Payload->SetBoolField(TEXT("includeValues"), true);
        Payload->SetBoolField(TEXT("includeDefault"), true);
        Payload->SetBoolField(TEXT("includeOverrideState"), true);
        return Payload;
    }

    FName MakeUniqueBlueprintName(const TCHAR* Prefix)
    {
        return FName(*FString::Printf(
            TEXT("%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyInheritedComponentParentTemplateDefaultTest,
    "PinWright.property.component_parent_default.InheritedComponentParentTemplateDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyInheritedComponentParentTemplateDefaultTest::RunTest(const FString& Parameters)
{
    const FName ParentName = MakeUniqueBlueprintName(TEXT("BP_PropertyParentDefault_Parent"));
    UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        ParentName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("parent blueprint created"), ParentBP);
    if (!ParentBP || !ParentBP->GeneratedClass || !ParentBP->SimpleConstructionScript)
    {
        return false;
    }

    USCS_Node* ParentComponentNode =
        ParentBP->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("InheritedScene"));
    TestNotNull(TEXT("parent SCS component node created"), ParentComponentNode);
    if (!ParentComponentNode || !ParentComponentNode->ComponentTemplate)
    {
        return false;
    }
    ParentBP->SimpleConstructionScript->AddNode(ParentComponentNode);

    UActorComponent* ParentTemplate = ParentComponentNode->ComponentTemplate;
    ParentTemplate->ComponentTags.Reset();
    ParentTemplate->ComponentTags.Add(TEXT("ParentTag"));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ParentBP);
    FKismetEditorUtilities::CompileBlueprint(ParentBP);
    TestNotNull(TEXT("parent generated class after compile"), ParentBP->GeneratedClass.Get());
    if (!ParentBP->GeneratedClass)
    {
        return false;
    }

    const FName ChildName = MakeUniqueBlueprintName(TEXT("BP_PropertyParentDefault_Child"));
    UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
        ParentBP->GeneratedClass,
        GetTransientPackage(),
        ChildName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("child blueprint created"), ChildBP);
    if (!ChildBP || !ChildBP->GeneratedClass)
    {
        return false;
    }

    UInheritableComponentHandler* ChildHandler = ChildBP->GetInheritableComponentHandler(true);
    TestNotNull(TEXT("child inheritable component handler created"), ChildHandler);
    if (!ChildHandler)
    {
        return false;
    }

    const FComponentKey ParentComponentKey(ParentComponentNode);
    UActorComponent* ChildTemplate = ChildHandler->CreateOverridenComponentTemplate(ParentComponentKey);
    TestNotNull(TEXT("child override component template created"), ChildTemplate);
    if (!ChildTemplate)
    {
        return false;
    }

    ChildTemplate->ComponentTags.Reset();
    ChildTemplate->ComponentTags.Add(TEXT("ChildTag"));

    FTestResponseCapture ListBeforeCapture;
    if (!InvokePropertyHandler(
        *this,
        TEXT("property.list"),
        MakeComponentTagsListPayload(ChildTemplate),
        ListBeforeCapture))
    {
        return false;
    }

    TSharedPtr<FJsonObject> TagsBefore =
        PropertyListTestHelpers::FindPropertyEntry(ListBeforeCapture.Result, TEXT("ComponentTags"));
    TestTrue(TEXT("property.list includes ComponentTags"), TagsBefore.IsValid());
    if (!TagsBefore.IsValid())
    {
        return false;
    }

    FString DefaultSource;
    TestTrue(
        TEXT("ComponentTags default source is parent_template"),
        TagsBefore->TryGetStringField(TEXT("defaultSource"), DefaultSource) &&
        DefaultSource == TEXT("parent_template"));
    TestTrue(
        TEXT("ComponentTags value starts with child override"),
        IsSingleStringArrayField(TagsBefore, TEXT("value"), TEXT("ChildTag")));
    TestTrue(
        TEXT("ComponentTags default value comes from parent template"),
        IsSingleStringArrayField(TagsBefore, TEXT("defaultValue"), TEXT("ParentTag")));

    bool bIsOverridden = false;
    TestTrue(
        TEXT("ComponentTags isOverridden field present before reset"),
        TagsBefore->TryGetBoolField(TEXT("isOverridden"), bIsOverridden));
    TestTrue(TEXT("ComponentTags reports overridden before reset"), bIsOverridden);

    TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
    GetPayload->SetStringField(TEXT("objectPath"), ChildTemplate->GetPathName());
    GetPayload->SetStringField(TEXT("propertyName"), TEXT("ComponentTags"));
    GetPayload->SetBoolField(TEXT("includeDefault"), true);
    GetPayload->SetBoolField(TEXT("includeOverrideState"), true);

    FTestResponseCapture GetCapture;
    if (!InvokePropertyHandler(*this, TEXT("property.get"), GetPayload, GetCapture))
    {
        return false;
    }
    TestTrue(
        TEXT("property.get default source is parent_template"),
        GetCapture.Result->TryGetStringField(TEXT("defaultSource"), DefaultSource) &&
        DefaultSource == TEXT("parent_template"));
    TestTrue(
        TEXT("property.get default value comes from parent template"),
        IsSingleStringArrayField(GetCapture.Result, TEXT("defaultValue"), TEXT("ParentTag")));

    TSharedPtr<FJsonObject> ResetPayload = MakeShared<FJsonObject>();
    ResetPayload->SetStringField(TEXT("objectPath"), ChildTemplate->GetPathName());
    ResetPayload->SetStringField(TEXT("propertyName"), TEXT("ComponentTags"));
    ResetPayload->SetBoolField(TEXT("markDirty"), false);

    FTestResponseCapture ResetCapture;
    if (!InvokePropertyHandler(*this, TEXT("property.reset"), ResetPayload, ResetCapture))
    {
        return false;
    }
    TestTrue(
        TEXT("property.reset default source is parent_template"),
        ResetCapture.Result->TryGetStringField(TEXT("defaultSource"), DefaultSource) &&
        DefaultSource == TEXT("parent_template"));
    TestTrue(
        TEXT("property.reset default value comes from parent template"),
        IsSingleStringArrayField(ResetCapture.Result, TEXT("defaultValue"), TEXT("ParentTag")));

    TestEqual(TEXT("reset leaves one component tag"), ChildTemplate->ComponentTags.Num(), 1);
    if (ChildTemplate->ComponentTags.Num() == 1)
    {
        TestEqual(TEXT("reset copied parent tag"), ChildTemplate->ComponentTags[0], FName(TEXT("ParentTag")));
    }

    FTestResponseCapture ListAfterCapture;
    if (!InvokePropertyHandler(
        *this,
        TEXT("property.list"),
        MakeComponentTagsListPayload(ChildTemplate),
        ListAfterCapture))
    {
        return false;
    }

    TSharedPtr<FJsonObject> TagsAfter =
        PropertyListTestHelpers::FindPropertyEntry(ListAfterCapture.Result, TEXT("ComponentTags"));
    TestTrue(TEXT("second property.list includes ComponentTags"), TagsAfter.IsValid());
    if (!TagsAfter.IsValid())
    {
        return false;
    }

    TestTrue(
        TEXT("second property.list default source is parent_template"),
        TagsAfter->TryGetStringField(TEXT("defaultSource"), DefaultSource) &&
        DefaultSource == TEXT("parent_template"));
    TestTrue(
        TEXT("second property.list value is parent tag"),
        IsSingleStringArrayField(TagsAfter, TEXT("value"), TEXT("ParentTag")));
    TestTrue(
        TEXT("second property.list default value is parent tag"),
        IsSingleStringArrayField(TagsAfter, TEXT("defaultValue"), TEXT("ParentTag")));

    bIsOverridden = true;
    TestTrue(
        TEXT("ComponentTags isOverridden field present after reset"),
        TagsAfter->TryGetBoolField(TEXT("isOverridden"), bIsOverridden));
    TestFalse(TEXT("ComponentTags reports not overridden after reset"), bIsOverridden);

    return true;
}

// ============================================================================
// PinWright.utils.property_utils.SCSAddedComponentTemplateExpandsOnCDO
//
// BP-added (SCS-tree) component variables are null on the CDO — the BPGC
// populates them at instance construction. The property dumper must look the
// template up via SimpleConstructionScript->FindSCSNode(VarName) and recurse
// into the ICH-aware archetype so the JSON output carries the authored state
// (here, ComponentTags) instead of `value: null`.
//
// Counterfactual: reverting ResolveSCSComponentTemplate causes
// GetObjectPropertyValue_InContainer(CDO) to return null, ExportPropertyToJsonValue
// emits FJsonValueNull, BuildClassPropertyJson surfaces it as `value: null`, and
// the ComponentTags lookup never finds the array.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsSCSAddedComponentTemplateExpandsOnCDOTest,
    "PinWright.utils.property_utils.SCSAddedComponentTemplateExpandsOnCDO",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsSCSAddedComponentTemplateExpandsOnCDOTest::RunTest(const FString& Parameters)
{
    const FName BlueprintName = MakeUniqueBlueprintName(TEXT("BP_PropertySCSExpand"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        BlueprintName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("blueprint created"), BP);
    if (!BP || !BP->SimpleConstructionScript)
    {
        return false;
    }

    USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("BoxLike"));
    TestNotNull(TEXT("SCS component node created"), Node);
    if (!Node || !Node->ComponentTemplate)
    {
        return false;
    }
    Node->ComponentTemplate->ComponentTags.Reset();
    Node->ComponentTemplate->ComponentTags.Add(TEXT("ScsTag"));
    BP->SimpleConstructionScript->AddNode(Node);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);
    TestNotNull(TEXT("generated class after compile"), BP->GeneratedClass.Get());
    if (!BP->GeneratedClass)
    {
        return false;
    }

    UObject* CDO = BP->GeneratedClass->GetDefaultObject();
    TestNotNull(TEXT("CDO available"), CDO);
    if (!CDO)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Result = BuildClassPropertyJson(CDO, nullptr);
    TestNotNull(TEXT("BuildClassPropertyJson returned non-null"), Result.Get());
    if (!Result)
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* BoxLikeEntry = nullptr;
    TestTrue(
        TEXT("Result contains BoxLike key"),
        Result->TryGetObjectField(TEXT("BoxLike"), BoxLikeEntry) && BoxLikeEntry && BoxLikeEntry->IsValid());
    if (!BoxLikeEntry || !BoxLikeEntry->IsValid())
    {
        return false;
    }

    TSharedPtr<FJsonValue> ValueField = (*BoxLikeEntry)->TryGetField(TEXT("value"));
    TestTrue(TEXT("BoxLike.value field present"), ValueField.IsValid());
    if (!ValueField.IsValid())
    {
        return false;
    }
    TestFalse(TEXT("BoxLike.value is not JSON null"), ValueField->Type == EJson::Null);
    TestEqual(TEXT("BoxLike.value is a JSON object"), static_cast<int32>(ValueField->Type), static_cast<int32>(EJson::Object));
    if (ValueField->Type != EJson::Object)
    {
        return false;
    }

    TSharedPtr<FJsonObject> ValueObj = ValueField->AsObject();
    TestTrue(TEXT("BoxLike.value object valid"), ValueObj.IsValid());
    if (!ValueObj.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
    TestTrue(
        TEXT("BoxLike.value.ComponentTags is an array"),
        ValueObj->TryGetArrayField(TEXT("ComponentTags"), TagsArr) && TagsArr);
    if (!TagsArr)
    {
        return false;
    }

    bool bFoundScsTag = false;
    for (const TSharedPtr<FJsonValue>& Tag : *TagsArr)
    {
        if (Tag.IsValid() && Tag->Type == EJson::String && Tag->AsString() == TEXT("ScsTag"))
        {
            bFoundScsTag = true;
            break;
        }
    }
    TestTrue(TEXT("BoxLike.value.ComponentTags contains ScsTag"), bFoundScsTag);

    return true;
}
