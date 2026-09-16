// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "Components/SceneComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/BillboardComponent.h"
#include "PinWrightHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateComponentCopiesInstanceAttachmentAndPropertiesTest,
    "PinWright.actor.duplicate_component.CopiesInstanceAttachmentAndProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FActorDuplicateComponentCopiesInstanceAttachmentAndPropertiesTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    const FString ActorLabel = FString::Printf(TEXT("PW_DuplicateComponent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AActor* Actor = SpawnActorInActiveWorld<AActor>(
        AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
    TestNotNull(TEXT("actor spawned"), Actor);
    if (!Actor)
    {
        return false;
    }
    USceneComponent* Root = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(), TEXT("RootForDuplicate"), RF_Transactional);
    TestNotNull(TEXT("root component created"), Root);
    if (!Root)
    {
        return false;
    }
    Actor->AddInstanceComponent(Root);
    Actor->SetRootComponent(Root);
    Root->RegisterComponent();

    USceneComponent* Source = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(), TEXT("SourceComponent"), RF_Transactional);
    TestNotNull(TEXT("source component created"), Source);
    if (!Source)
    {
        return false;
    }
    Actor->AddInstanceComponent(Source);
    Source->SetupAttachment(Root);
    Source->SetRelativeLocation(FVector(11.0, 22.0, 33.0));
    Source->SetRelativeRotation(FRotator(10.0, 20.0, 30.0));
    Source->SetRelativeScale3D(FVector(2.0, 3.0, 4.0));
    Source->ComponentTags.Add(TEXT("CopiedInstanceTag"));
    Source->SetVisibility(false);
    Source->RegisterComponent();

    USceneComponent* SourceChild = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(), TEXT("SourceChildComponent"), RF_Transactional);
    TestNotNull(TEXT("source child component created"), SourceChild);
    if (!SourceChild)
    {
        return false;
    }
    Actor->AddInstanceComponent(SourceChild);
    SourceChild->SetupAttachment(Source);
    SourceChild->SetRelativeLocation(FVector(1.0, 2.0, 3.0));
    SourceChild->RegisterComponent();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ActorLabel);
    Payload->SetStringField(TEXT("sourceName"), TEXT("SourceComponent"));
    Payload->SetStringField(TEXT("newName"), TEXT("SourceComponent_Copy"));
    Payload->SetBoolField(TEXT("duplicateChildren"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.duplicate_component handler found"),
        InvokeHandlerWithCapture(TEXT("actor.duplicate_component"), Payload, Capture));
    TestTrue(TEXT("actor.duplicate_component succeeded"), Capture.bSuccess);

    USceneComponent* Duplicate = Cast<USceneComponent>(
        FindActorComponentByName(Actor, TEXT("SourceComponent_Copy")));

    TestNotNull(TEXT("duplicate component exists"), Duplicate);
    if (!Duplicate)
    {
        return false;
    }

    TestEqual(TEXT("duplicate class copied"), Duplicate->GetClass(), Source->GetClass());
    TestEqual(TEXT("relative location copied"), Duplicate->GetRelativeLocation(), FVector(11.0, 22.0, 33.0));
    TestEqual(TEXT("relative rotation copied"), Duplicate->GetRelativeRotation(), FRotator(10.0, 20.0, 30.0));
    TestEqual(TEXT("relative scale copied"), Duplicate->GetRelativeScale3D(), FVector(2.0, 3.0, 4.0));
    TestTrue(TEXT("component tag copied"), Duplicate->ComponentTags.Contains(TEXT("CopiedInstanceTag")));
    TestFalse(TEXT("visibility copied"), Duplicate->IsVisible());
    TestEqual(TEXT("attachment parent copied"), Duplicate->GetAttachParent(), Root);

    USceneComponent* DuplicateChild = nullptr;
    for (USceneComponent* Child : Duplicate->GetAttachChildren())
    {
        if (Child && Child->GetName().StartsWith(TEXT("SourceChildComponent_Copy")))
        {
            DuplicateChild = Child;
            break;
        }
    }
    TestNotNull(TEXT("attached child duplicated"), DuplicateChild);
    if (DuplicateChild)
    {
        TestEqual(TEXT("child relative location copied"), DuplicateChild->GetRelativeLocation(), FVector(1.0, 2.0, 3.0));
    }

    TestTrue(TEXT("mapping returned"), Capture.Result.IsValid() && Capture.Result->HasTypedField<EJson::Object>(TEXT("mapping")));
    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject> Mapping = Capture.Result->GetObjectField(TEXT("mapping"));
        FString MappedName;
        Mapping->TryGetStringField(TEXT("SourceComponent"), MappedName);
        TestEqual(TEXT("root mapping returned"), MappedName, FString(TEXT("SourceComponent_Copy")));
    }

    return true;
}

// Counts the billboard sprite children directly attached to Component. In the editor
// world USceneComponent::CreateSpriteComponent auto-attaches exactly one transient
// UBillboardComponent to a light on register; a redundant clone made by
// duplicate_component's recursion would add a second one. A bare AActor + PointLight
// fixture only ever carries this auto-sprite billboard, so a plain UBillboardComponent
// Cast is the authoritative discriminator — deliberately NOT re-checking
// IsVisualizationComponent()/IsEditorOnly() here, so the assertion characterizes the
// observed sprite independently of the production skip predicate it is meant to verify.
static int32 CountBillboardChildren(USceneComponent* Component)
{
    int32 Count = 0;
    if (!Component)
    {
        return Count;
    }
    for (USceneComponent* Child : Component->GetAttachChildren())
    {
        if (Cast<UBillboardComponent>(Child))
        {
            ++Count;
        }
    }
    return Count;
}

// Regression for B-duplicate-component-clones-editor-sprite: actor.duplicate_component with
// the default duplicateChildren:true must NOT clone the engine's auto-created editor-only
// light sprite. The source light auto-attaches a transient visualization UBillboardComponent
// on register; the duplicate regenerates its OWN sprite on RegisterComponent(), so cloning
// the source's sprite yields a redundant persistent instance component (two billboards per
// duplicated light) and leaks a BillboardComponent_*_Copy_* into the response mapping.
// The fixture is built entirely in code (a bare AActor + a PointLightComponent) — it loads
// no external content. Reverting the editor-only/visualization skip in the child-recursion
// loop makes both the "no cloned sprite" child assertion and the "mapping has no billboard
// entry" assertion fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDuplicateComponentSkipsEditorSpriteChildTest,
    "PinWright.actor.duplicate_component.SkipsEditorOnlyLightSpriteChild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FActorDuplicateComponentSkipsEditorSpriteChildTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    const FString ActorLabel = FString::Printf(TEXT("PW_DuplicateLightSprite_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AActor* Actor = SpawnActorInActiveWorld<AActor>(
        AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
    TestNotNull(TEXT("actor spawned"), Actor);
    if (!Actor)
    {
        return false;
    }
    // The light auto-attaches its editor-only visualization sprite only in a non-game
    // (editor) world. A game world is a broken fixture, not a skip — fail loudly.
    UWorld* World = Actor->GetWorld();
    TestNotNull(TEXT("actor has a world"), World);
    if (!World)
    {
        return false;
    }
    TestFalse(TEXT("fixture world is an editor (non-game) world so the sprite auto-creates"),
        World->IsGameWorld());
    if (World->IsGameWorld())
    {
        return false;
    }

    USceneComponent* Root = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(), TEXT("SpriteRoot"), RF_Transactional);
    TestNotNull(TEXT("root component created"), Root);
    if (!Root)
    {
        return false;
    }
    Actor->AddInstanceComponent(Root);
    Actor->SetRootComponent(Root);
    Root->RegisterComponent();

    UPointLightComponent* Light = NewObject<UPointLightComponent>(Actor, UPointLightComponent::StaticClass(), TEXT("SourceLight"), RF_Transactional);
    TestNotNull(TEXT("point light component created"), Light);
    if (!Light)
    {
        return false;
    }
    Actor->AddInstanceComponent(Light);
    Light->SetupAttachment(Root);
    // RegisterComponent triggers USceneComponent::OnRegister -> CreateSpriteComponent,
    // which auto-attaches the editor-only visualization billboard to the light.
    Light->RegisterComponent();

    // Fixture sanity: the engine must have auto-created exactly one visualization sprite
    // child on the source light. If it did not, the fixture is invalid (e.g. sprites are
    // disabled in this run) and the regression cannot be exercised — that is a FAILURE, not
    // a skip.
    const int32 SourceSpriteChildren = CountBillboardChildren(Light);
    TestEqual(TEXT("source light auto-created exactly one editor sprite child"), SourceSpriteChildren, 1);
    if (SourceSpriteChildren != 1)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ActorLabel);
    Payload->SetStringField(TEXT("sourceName"), TEXT("SourceLight"));
    Payload->SetStringField(TEXT("newName"), TEXT("SourceLight_Copy"));
    // Exercise the default child-recursion path — the bug lives here.
    Payload->SetBoolField(TEXT("duplicateChildren"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.duplicate_component handler found"),
        InvokeHandlerWithCapture(TEXT("actor.duplicate_component"), Payload, Capture));
    TestTrue(TEXT("actor.duplicate_component succeeded"), Capture.bSuccess);

    UPointLightComponent* Duplicate = Cast<UPointLightComponent>(
        FindActorComponentByName(Actor, TEXT("SourceLight_Copy")));
    TestNotNull(TEXT("duplicate light exists"), Duplicate);
    if (!Duplicate)
    {
        return false;
    }

    // Core assertion: the duplicate regenerated its OWN sprite on register, so it should
    // carry exactly one visualization billboard child. Pre-fix the recursion ALSO cloned
    // the source's sprite, leaving two. (>= 2 is the defect.)
    const int32 DuplicateSpriteChildren = CountBillboardChildren(Duplicate);
    TestEqual(TEXT("duplicate light has exactly one sprite child (its own auto-sprite, no clone)"),
        DuplicateSpriteChildren, 1);

    // The response mapping must not report a cloned billboard. Pre-fix it carried an entry
    // like "BillboardComponent_0" -> "BillboardComponent_0_Copy_0"; post-fix only the light
    // itself is mapped.
    TestTrue(TEXT("mapping returned"),
        Capture.Result.IsValid() && Capture.Result->HasTypedField<EJson::Object>(TEXT("mapping")));
    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject> Mapping = Capture.Result->GetObjectField(TEXT("mapping"));
        bool bMappingHasBillboard = false;
        for (const TPair<FString, TSharedPtr<FJsonValue>> Entry : Mapping->Values)
        {
            if (Entry.Key.Contains(TEXT("Billboard")))
            {
                bMappingHasBillboard = true;
                break;
            }
        }
        TestFalse(TEXT("mapping contains no cloned billboard sprite entry"), bMappingHasBillboard);
    }

    return true;
}
