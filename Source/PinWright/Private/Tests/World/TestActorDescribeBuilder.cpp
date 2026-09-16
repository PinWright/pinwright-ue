// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/ActorDescribeBuilder.h"
#include "Utils/ComponentReadFilter.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TimelineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"


namespace
{
    TSharedPtr<FJsonObject> FindComponentJson(
        const TArray<TSharedPtr<FJsonValue>>& Components,
        const FString& ComponentName)
    {
        for (const TSharedPtr<FJsonValue>& ComponentValue : Components)
        {
            const TSharedPtr<FJsonObject> ComponentObject =
                ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
            FString Name;
            if (ComponentObject.IsValid()
                && ComponentObject->TryGetStringField(TEXT("name"), Name)
                && Name == ComponentName)
            {
                return ComponentObject;
            }
        }
        return nullptr;
    }

    bool HasObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        return Object.IsValid() && Object->HasTypedField<EJson::Object>(FieldName);
    }

    bool HasStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        FString Value;
        return Object.IsValid() && Object->TryGetStringField(FieldName, Value);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDescribeBuilderShapeTest,
    "PinWright.actor.describe_builder.ShapeAndSparseProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDescribeBuilderShapeTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AActor* Actor = NewObject<AActor>(
        GetTransientPackage(),
        AActor::StaticClass(),
        *FString::Printf(TEXT("PW_ActorDescribe_%s"), *Suffix),
        RF_Transient);
    TestNotNull(TEXT("transient actor created"), Actor);
    if (!Actor)
    {
        return false;
    }

    Actor->SetActorHiddenInGame(true);
    Actor->Tags.Add(TEXT("ActorDescribeTag"));

    USceneComponent* Root = NewObject<USceneComponent>(
        Actor,
        USceneComponent::StaticClass(),
        TEXT("DescribeRoot"),
        RF_Transient);
    USceneComponent* Child = NewObject<USceneComponent>(
        Actor,
        USceneComponent::StaticClass(),
        TEXT("DescribeChild"),
        RF_Transient);
    UTimelineComponent* NonScene = NewObject<UTimelineComponent>(
        Actor,
        UTimelineComponent::StaticClass(),
        TEXT("DescribeLogic"),
        RF_Transient);

    TestNotNull(TEXT("root scene component created"), Root);
    TestNotNull(TEXT("child scene component created"), Child);
    TestNotNull(TEXT("non-scene component created"), NonScene);
    if (!Root || !Child || !NonScene)
    {
        return false;
    }

    Actor->AddInstanceComponent(Root);
    Actor->SetRootComponent(Root);
    Root->SetRelativeLocation(FVector(10.0, 20.0, 30.0));
    Root->ComponentTags.Add(TEXT("RootTag"));

    Actor->AddInstanceComponent(Child);
    Child->SetupAttachment(Root);
    Child->SetRelativeLocation(FVector(1.0, 2.0, 3.0));
    Child->SetRelativeRotation(FRotator(4.0, 5.0, 6.0));
    Child->SetRelativeScale3D(FVector(1.5, 2.0, 2.5));
    Child->SetVisibility(false);
    Child->ComponentTags.Add(TEXT("ChildTag"));

    Actor->AddInstanceComponent(NonScene);
    NonScene->ComponentTags.Add(TEXT("LogicTag"));

    TSharedPtr<FJsonObject> Result = ActorDescribeBuilder::BuildActorJson(Actor, TEXT("test"));
    TestTrue(TEXT("actor describe JSON is valid"), Result.IsValid());
    if (!Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("schema"),
        Result->GetStringField(TEXT("schema")),
        FString(TEXT("pinwright.actor-describe.v1")));
    TestEqual(TEXT("storage"), Result->GetStringField(TEXT("storage")), FString(TEXT("test")));
    TestTrue(TEXT("name exists"), HasStringField(Result, TEXT("name")));
    TestTrue(TEXT("label exists"), HasStringField(Result, TEXT("label")));
    TestTrue(TEXT("path exists"), HasStringField(Result, TEXT("path")));
    TestTrue(TEXT("class exists"), HasStringField(Result, TEXT("class")));
    TestTrue(TEXT("transform exists"), HasObjectField(Result, TEXT("transform")));
    TestTrue(TEXT("properties exists"), HasObjectField(Result, TEXT("properties")));

    const TSharedPtr<FJsonObject> ActorProperties = Result->GetObjectField(TEXT("properties"));
    TestTrue(TEXT("changed actor property is included"),
        ActorProperties.IsValid() && ActorProperties->HasField(TEXT("bHidden")));
    TestFalse(TEXT("unchanged actor property is omitted"),
        ActorProperties.IsValid() && ActorProperties->HasField(TEXT("bReplicates")));

    const TArray<TSharedPtr<FJsonValue>>& Components = Result->GetArrayField(TEXT("components"));
    TestEqual(TEXT("all fixture components exported"), Components.Num(), 3);

    TSharedPtr<FJsonObject> RootJson = FindComponentJson(Components, TEXT("DescribeRoot"));
    TSharedPtr<FJsonObject> ChildJson = FindComponentJson(Components, TEXT("DescribeChild"));
    TSharedPtr<FJsonObject> NonSceneJson = FindComponentJson(Components, TEXT("DescribeLogic"));

    TestTrue(TEXT("root component exported"), RootJson.IsValid());
    TestTrue(TEXT("child component exported"), ChildJson.IsValid());
    TestTrue(TEXT("non-scene component exported"), NonSceneJson.IsValid());
    if (!RootJson.IsValid() || !ChildJson.IsValid() || !NonSceneJson.IsValid())
    {
        return false;
    }

    for (const TSharedPtr<FJsonObject>& ComponentJson : { RootJson, ChildJson, NonSceneJson })
    {
        TestTrue(TEXT("component name exists"), HasStringField(ComponentJson, TEXT("name")));
        TestTrue(TEXT("component path exists"), HasStringField(ComponentJson, TEXT("path")));
        TestTrue(TEXT("component class exists"), HasStringField(ComponentJson, TEXT("class")));
        TestTrue(TEXT("component creationMethod exists"), HasStringField(ComponentJson, TEXT("creationMethod")));
        TestTrue(TEXT("component tags exists"), ComponentJson->HasTypedField<EJson::Array>(TEXT("tags")));
        TestTrue(TEXT("component properties exists"), HasObjectField(ComponentJson, TEXT("properties")));
    }

    TestTrue(TEXT("root scene relativeTransform exists"),
        HasObjectField(RootJson, TEXT("relativeTransform")));
    TestTrue(TEXT("child scene relativeTransform exists"),
        HasObjectField(ChildJson, TEXT("relativeTransform")));
    FString AttachParentName;
    TestTrue(TEXT("child attachParentName exists"),
        ChildJson->TryGetStringField(TEXT("attachParentName"), AttachParentName));
    TestEqual(TEXT("child attachParentName"), AttachParentName, FString(TEXT("DescribeRoot")));

    const TSharedPtr<FJsonObject> ChildProperties = ChildJson->GetObjectField(TEXT("properties"));
    TestTrue(TEXT("changed component property is included"),
        ChildProperties.IsValid() && ChildProperties->HasField(TEXT("bVisible")));
    TestFalse(TEXT("scene transform property is omitted from sparse component properties"),
        ChildProperties.IsValid() && ChildProperties->HasField(TEXT("RelativeLocation")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDescribeBuilderComponentFilterTest,
    "PinWright.actor.describe_builder.ComponentFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDescribeBuilderComponentFilterTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AActor* Actor = NewObject<AActor>(
        GetTransientPackage(),
        AActor::StaticClass(),
        *FString::Printf(TEXT("PW_ActorDescribeFilter_%s"), *Suffix),
        RF_Transient);
    TestNotNull(TEXT("transient actor created"), Actor);
    if (!Actor)
    {
        return false;
    }

    USceneComponent* Root = NewObject<USceneComponent>(
        Actor,
        USceneComponent::StaticClass(),
        TEXT("FilterRoot"),
        RF_Transient);
    UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(
        Actor,
        UStaticMeshComponent::StaticClass(),
        TEXT("FilterWantedMesh"),
        RF_Transient);
    UTimelineComponent* Logic = NewObject<UTimelineComponent>(
        Actor,
        UTimelineComponent::StaticClass(),
        TEXT("FilterLogic"),
        RF_Transient);

    TestNotNull(TEXT("root scene component created"), Root);
    TestNotNull(TEXT("mesh scene component created"), Mesh);
    TestNotNull(TEXT("non-scene component created"), Logic);
    if (!Root || !Mesh || !Logic)
    {
        return false;
    }

    Actor->AddInstanceComponent(Root);
    Actor->SetRootComponent(Root);
    Actor->AddInstanceComponent(Mesh);
    Mesh->SetupAttachment(Root);
    Actor->AddInstanceComponent(Logic);

    FComponentReadFilter NameFilter;
    NameFilter.NameMatch = TEXT("wanted");
    const TArray<TSharedPtr<FJsonValue>> NameFiltered =
        ActorDescribeBuilder::BuildComponentsJson(Actor, NameFilter);
    TestEqual(TEXT("nameMatch reduces components to one"), NameFiltered.Num(), 1);
    TestTrue(TEXT("nameMatch returns the matching component"),
        FindComponentJson(NameFiltered, TEXT("FilterWantedMesh")).IsValid());

    FComponentReadFilter ClassFilter;
    ClassFilter.ComponentClass = USceneComponent::StaticClass();
    const TArray<TSharedPtr<FJsonValue>> ClassFiltered =
        ActorDescribeBuilder::BuildComponentsJson(Actor, ClassFilter);
    TestEqual(TEXT("componentClass includes subclasses"), ClassFiltered.Num(), 2);
    TestTrue(TEXT("class filter includes root scene component"),
        FindComponentJson(ClassFiltered, TEXT("FilterRoot")).IsValid());
    TestTrue(TEXT("class filter includes static mesh component subclass"),
        FindComponentJson(ClassFiltered, TEXT("FilterWantedMesh")).IsValid());
    TestFalse(TEXT("class filter excludes non-scene actor component"),
        FindComponentJson(ClassFiltered, TEXT("FilterLogic")).IsValid());

    return true;
}

// Regression for E-actor-describe-no-header-only-read: the fields allow-list and
// includeComponents:false read-shaping on BuildActorJson. Counterfactual: revert
// the projection in BuildActorJson and the field-projection case keeps guid /
// properties / components (5 failing checks) while includeComponents:false keeps
// the components array.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDescribeBuilderFieldProjectionTest,
    "PinWright.actor.describe_builder.FieldProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDescribeBuilderFieldProjectionTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AActor* Actor = NewObject<AActor>(
        GetTransientPackage(),
        AActor::StaticClass(),
        *FString::Printf(TEXT("PW_ActorDescribeProj_%s"), *Suffix),
        RF_Transient);
    TestNotNull(TEXT("transient actor created"), Actor);
    if (!Actor)
    {
        return false;
    }

    USceneComponent* Root = NewObject<USceneComponent>(
        Actor,
        USceneComponent::StaticClass(),
        TEXT("ProjRoot"),
        RF_Transient);
    UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(
        Actor,
        UStaticMeshComponent::StaticClass(),
        TEXT("ProjMesh"),
        RF_Transient);
    TestNotNull(TEXT("root scene component created"), Root);
    TestNotNull(TEXT("mesh component created"), Mesh);
    if (!Root || !Mesh)
    {
        return false;
    }
    Actor->AddInstanceComponent(Root);
    Actor->SetRootComponent(Root);
    Actor->AddInstanceComponent(Mesh);
    Mesh->SetupAttachment(Root);

    // Baseline (no options): the full shape carries all top-level keys.
    const TSharedPtr<FJsonObject> Full = ActorDescribeBuilder::BuildActorJson(Actor, TEXT("test"));
    TestTrue(TEXT("baseline has components"), Full.IsValid() && Full->HasField(TEXT("components")));
    TestTrue(TEXT("baseline has guid"), Full.IsValid() && Full->HasField(TEXT("guid")));
    TestTrue(TEXT("baseline has properties"), Full.IsValid() && Full->HasField(TEXT("properties")));

    // fields allow-list: project down to {label, transform}. schema/storage are
    // always retained as the envelope; everything else is dropped.
    FActorDescribeOptions FieldOptions;
    FieldOptions.Fields = { TEXT("label"), TEXT("transform") };
    const TSharedPtr<FJsonObject> Projected =
        ActorDescribeBuilder::BuildActorJson(Actor, TEXT("test"), FComponentReadFilter(), FieldOptions);
    TestTrue(TEXT("projected JSON valid"), Projected.IsValid());
    if (!Projected.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("projected keeps label"), Projected->HasField(TEXT("label")));
    TestTrue(TEXT("projected keeps transform"), Projected->HasField(TEXT("transform")));
    TestTrue(TEXT("projected keeps schema envelope"), Projected->HasField(TEXT("schema")));
    TestTrue(TEXT("projected keeps storage envelope"), Projected->HasField(TEXT("storage")));
    TestFalse(TEXT("projected drops components"), Projected->HasField(TEXT("components")));
    TestFalse(TEXT("projected drops properties"), Projected->HasField(TEXT("properties")));
    TestFalse(TEXT("projected drops guid"), Projected->HasField(TEXT("guid")));
    TestFalse(TEXT("projected drops name (not requested)"), Projected->HasField(TEXT("name")));
    TestEqual(TEXT("projected key count is exactly label+transform+schema+storage"),
        Projected->Values.Num(), 4);

    // fields is case-insensitive on the requested keys.
    FActorDescribeOptions CaseOptions;
    CaseOptions.Fields = { TEXT("LABEL") };
    const TSharedPtr<FJsonObject> CaseProjected =
        ActorDescribeBuilder::BuildActorJson(Actor, TEXT("test"), FComponentReadFilter(), CaseOptions);
    TestTrue(TEXT("case-insensitive fields keeps label"),
        CaseProjected.IsValid() && CaseProjected->HasField(TEXT("label")));

    // fields can explicitly whitelist components.
    FActorDescribeOptions ComponentsField;
    ComponentsField.Fields = { TEXT("components") };
    const TSharedPtr<FJsonObject> ComponentsProjected =
        ActorDescribeBuilder::BuildActorJson(Actor, TEXT("test"), FComponentReadFilter(), ComponentsField);
    TestTrue(TEXT("fields=[components] keeps components"),
        ComponentsProjected.IsValid() && ComponentsProjected->HasField(TEXT("components")));
    TestFalse(TEXT("fields=[components] drops transform"),
        ComponentsProjected.IsValid() && ComponentsProjected->HasField(TEXT("transform")));

    // includeComponents:false drops only the component array; the header stays.
    FActorDescribeOptions NoComponents;
    NoComponents.bIncludeComponents = false;
    const TSharedPtr<FJsonObject> HeaderOnly =
        ActorDescribeBuilder::BuildActorJson(Actor, TEXT("test"), FComponentReadFilter(), NoComponents);
    TestTrue(TEXT("includeComponents:false JSON valid"), HeaderOnly.IsValid());
    if (!HeaderOnly.IsValid())
    {
        return false;
    }
    TestFalse(TEXT("includeComponents:false drops components"), HeaderOnly->HasField(TEXT("components")));
    TestTrue(TEXT("includeComponents:false keeps transform"), HeaderOnly->HasField(TEXT("transform")));
    TestTrue(TEXT("includeComponents:false keeps label"), HeaderOnly->HasField(TEXT("label")));
    TestTrue(TEXT("includeComponents:false keeps properties"), HeaderOnly->HasField(TEXT("properties")));

    return true;
}
