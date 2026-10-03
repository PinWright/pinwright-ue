// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-wiki-hism-recipe-resets-actor-transform.
//
// The wiki's owned-HISM recipe (docs/wiki-src/level-building.instancing-and-scatter.md) used to
// make a new_object'd HISM the holder's root by reflection. An actor's location IS its root's
// location, and the spawn location lived on the DefaultSceneRoot the editor's empty-actor factory
// creates (UActorFactoryEmptyActor::SpawnActor), so the swap moved the whole layer to the world
// origin. The recipe now spawns the holder the same way and adds the HISM with actor.add_component.
//
// This test runs the recipe's own steps from C++ against the real factory and the real verb, and
// pins what the page promises: the holder keeps its placement, the HISM is attached under the
// factory root rather than replacing it, it is registered, meshPath reaches it, and an instance
// written relative to the holder lands relative to the anchor in world space.

#include "Misc/AutomationTest.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightHismHolderRecipeTest
{
    const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // Trailing 'X' so FActorLabelUtilities cannot strip a numeric tail and uniquify the label.
    FString MakeHolderLabel()
    {
        return FString::Printf(TEXT("PWHismHolder_%sX"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHismHolderRecipeKeepsPlacementTest,
    "PinWright.actor.add_component.HismUnderFactoryRootKeepsPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHismHolderRecipeKeepsPlacementTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightHismHolderRecipeTest;

    UEditorActorSubsystem* ActorSubsystem =
        GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
    if (!ActorSubsystem || !Cube)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No editor actor subsystem or engine Cube asset; skipping HismUnderFactoryRootKeepsPlacement."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;

    // Step 1 of the recipe: Python's spawn_actor_from_class is this call.
    const FVector Anchor(120000.0, -45000.0, 250.0);
    AActor* Holder = ActorSubsystem->SpawnActorFromClass(AActor::StaticClass(), Anchor);
    if (!Holder)
    {
        AddError(TEXT("SpawnActorFromClass(AActor) returned no actor."));
        return false;
    }
    const FString Label = MakeHolderLabel();
    Holder->SetActorLabel(Label);

    // Fixture preconditions: the factory root exists and carries the anchor. If these fail the
    // recipe's premise is wrong, not actor.add_component.
    USceneComponent* FactoryRoot = Holder->GetRootComponent();
    if (!FactoryRoot)
    {
        AddError(TEXT("Precondition: the empty-actor factory gave the holder no root component."));
        return false;
    }
    TestTrue(TEXT("precondition: the holder sits at the anchor before the component is added"),
        Holder->GetActorLocation().Equals(Anchor, 1.0));

    // Step 2: the verb, exactly as the page writes it.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("componentName"), TEXT("HISM_RecipeProbe"));
    Payload->SetStringField(TEXT("componentType"), TEXT("HierarchicalInstancedStaticMeshComponent"));
    Payload->SetStringField(TEXT("meshPath"), CubePath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.add_component is registered"),
        InvokeHandlerWithCapture(TEXT("actor.add_component"), Payload, Capture));
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("actor.add_component failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    UHierarchicalInstancedStaticMeshComponent* Hism = nullptr;
    TArray<UHierarchicalInstancedStaticMeshComponent*> Found;
    Holder->GetComponents(Found);
    for (UHierarchicalInstancedStaticMeshComponent* Candidate : Found)
    {
        if (Candidate && Candidate->GetName() == TEXT("HISM_RecipeProbe"))
        {
            Hism = Candidate;
        }
    }
    if (!Hism)
    {
        AddError(TEXT("The holder has no HISM named HISM_RecipeProbe after actor.add_component."));
        return false;
    }

    // THE REGRESSION: the root is untouched, so the placement survives.
    TestTrue(TEXT("the factory root is still the holder's root"), Holder->GetRootComponent() == FactoryRoot);
    TestTrue(TEXT("the holder still sits at the anchor"), Holder->GetActorLocation().Equals(Anchor, 1.0));
    TestTrue(TEXT("the HISM is attached under the factory root"), Hism->GetAttachParent() == FactoryRoot);
    TestTrue(TEXT("the HISM is registered from C++"), Hism->IsRegistered());
    TestTrue(TEXT("meshPath reaches a HISM, not only a plain StaticMeshComponent"), Hism->GetStaticMesh() == Cube);

    // Step 3: an instance written relative to the holder lands relative to the anchor.
    const FVector Offset(300.0, 0.0, 0.0);
    Hism->AddInstance(FTransform(Offset), /*bWorldSpace=*/false);
    FTransform InstanceWorld;
    TestTrue(TEXT("instance 0 reads back"), Hism->GetInstanceTransform(0, InstanceWorld, /*bWorldSpace=*/true));
    TestTrue(TEXT("a relative instance lands at anchor + offset in world space"),
        InstanceWorld.GetLocation().Equals(Anchor + Offset, 1.0));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
