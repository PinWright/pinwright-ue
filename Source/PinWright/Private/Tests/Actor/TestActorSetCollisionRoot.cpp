// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-set-collision-nonprimitive-root-silent-success.
//
// actor.set_collision wrote inside two bare ifs (root exists, root is a primitive) and then
// answered success with the REQUESTED collisionEnabled echoed back. An actor with no root, or
// with a plain scene root such as the DefaultSceneRoot every wiki HISM holder carries, got a
// success with nothing written. These tests drive the real dispatcher and pin the three
// outcomes: refusal for a non-primitive root, refusal for no root, and a success whose
// collisionEnabled is read back off the written component rather than echoed.
//
// Fixture actors are NOT RF_Transient: actor.* verbs resolve through
// UEditorActorSubsystem::GetAllLevelActors, which skips transient actors.

#include "Misc/AutomationTest.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightSetCollisionRootTest
{
    const FVector ProbeOrigin(0.0, 0.0, 12000.0);

    UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    struct FOutcome
    {
        bool bSuccess = false;
        FString ErrorCode;
        FString Message;
        TSharedPtr<FJsonObject> Result;
    };

    FOutcome SetCollision(const FString& ActorName, bool bEnabled)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), ActorName);
        Params->SetBoolField(TEXT("collisionEnabled"), bEnabled);

        FOutcome Out;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.set_collision"),
            TEXT("req-set-collision-root"), Params, Out.bSuccess, Out.Result, Out.ErrorCode);
        Out.Message = Sink->Message;
        return Out;
    }

    FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        FString Value;
        if (Object.IsValid())
        {
            Object->TryGetStringField(Field, Value);
        }
        return Value;
    }
}

// The wiki scatter-recipe shape: a plain AActor whose root is a USceneComponent named
// DefaultSceneRoot, with an HISM attached under it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetCollisionRefusesNonPrimitiveRootTest,
    "PinWright.actor.set_collision.RefusesNonPrimitiveRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetCollisionRefusesNonPrimitiveRootTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSetCollisionRootTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the holder actor cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), ProbeOrigin, FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("holder actor spawned"), Holder))
    {
        return false;
    }
    USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("DefaultSceneRoot"), RF_Transactional);
    Holder->SetRootComponent(Root);
    Holder->AddInstanceComponent(Root);
    Root->RegisterComponent();
    UHierarchicalInstancedStaticMeshComponent* Hism =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, TEXT("PWSetCollisionHism"), RF_Transactional);
    Hism->SetupAttachment(Root);
    Holder->AddInstanceComponent(Hism);
    Hism->RegisterComponent();
    Hism->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

    // Fixture preconditions: the root is a scene component that is not a primitive, and the
    // child primitive starts with collision on.
    if (!TestTrue(TEXT("holder root is the non-primitive DefaultSceneRoot"),
            Holder->GetRootComponent() == Root && !Root->IsA<UPrimitiveComponent>()))
    {
        return false;
    }
    TestEqual(TEXT("HISM starts with QueryAndPhysics"),
        Hism->GetCollisionEnabled(), ECollisionEnabled::Type(ECollisionEnabled::QueryAndPhysics));

    const FOutcome Out = SetCollision(Holder->GetName(), false);
    TestFalse(TEXT("set_collision on a non-primitive root is not a success"), Out.bSuccess);
    TestEqual(TEXT("error code is NO_COMPONENT"), Out.ErrorCode, FString(TEXT("NO_COMPONENT")));
    TestEqual(TEXT("error data names the root class"),
        StringField(Out.Result, TEXT("rootComponentClass")), FString(TEXT("SceneComponent")));
    const TArray<TSharedPtr<FJsonValue>>* Prims = nullptr;
    TestTrue(TEXT("error data lists the HISM as an addressable primitive component"),
        Out.Result.IsValid() && Out.Result->TryGetArrayField(TEXT("primitiveComponents"), Prims)
            && Prims->Num() == 1 && (*Prims)[0]->AsString() == Hism->GetName());
    TestTrue(TEXT("refusal names the working BodyInstance remedy"),
        Out.Message.Contains(TEXT("{actorName, componentName, properties:{BodyInstance:{CollisionEnabled:")));
    TestEqual(TEXT("the HISM under the root was not touched"),
        Hism->GetCollisionEnabled(), ECollisionEnabled::Type(ECollisionEnabled::QueryAndPhysics));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetCollisionRefusesRootlessActorTest,
    "PinWright.actor.set_collision.RefusesRootlessActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetCollisionRefusesRootlessActorTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSetCollisionRootTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the rootless actor cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    AActor* Rootless = World->SpawnActor<AActor>(AActor::StaticClass(), ProbeOrigin, FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("rootless actor spawned"), Rootless))
    {
        return false;
    }
    if (!TestNull(TEXT("fixture precondition: the actor has no root component"), Rootless->GetRootComponent()))
    {
        return false;
    }

    const FOutcome Out = SetCollision(Rootless->GetName(), true);
    TestFalse(TEXT("set_collision on a rootless actor is not a success"), Out.bSuccess);
    TestEqual(TEXT("error code is NO_COMPONENT"), Out.ErrorCode, FString(TEXT("NO_COMPONENT")));
    TestEqual(TEXT("error data reports an empty root class"),
        StringField(Out.Result, TEXT("rootComponentClass")), FString());
    TestTrue(TEXT("rootless refusal gives a next step"),
        Out.Message.Contains(TEXT("actor.add_component")));
    return true;
}

// On a primitive root the write lands, and the reply reports what the component now says.
// The second call is the discriminating one: with actor-level collision off,
// UPrimitiveComponent::GetCollisionEnabled reads NoCollision even after an enable, so a
// request echo answers true and a read-back answers false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetCollisionEchoesReadBackTest,
    "PinWright.actor.set_collision.EchoesReadBackCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetCollisionEchoesReadBackTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSetCollisionRootTest;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world context; the static mesh actor cannot be placed."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    AStaticMeshActor* MeshActor = World->SpawnActor<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), ProbeOrigin, FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("static mesh actor spawned"), MeshActor))
    {
        return false;
    }
    UStaticMeshComponent* Smc = MeshActor->GetStaticMeshComponent();
    if (!TestTrue(TEXT("fixture precondition: root is the static mesh component"),
            Smc && MeshActor->GetRootComponent() == Smc))
    {
        return false;
    }

    const FOutcome Disable = SetCollision(MeshActor->GetName(), false);
    TestTrue(TEXT("disable on a primitive root succeeds"), Disable.bSuccess);
    TestEqual(TEXT("reply names the written component"),
        StringField(Disable.Result, TEXT("componentName")), Smc->GetName());
    bool bReported = true;
    TestTrue(TEXT("reply carries collisionEnabled"),
        Disable.Result.IsValid() && Disable.Result->TryGetBoolField(TEXT("collisionEnabled"), bReported));
    TestFalse(TEXT("reply reports collision disabled"), bReported);
    TestFalse(TEXT("a read-back that matches the request carries no warnings"),
        Disable.Result.IsValid() && Disable.Result->HasField(TEXT("warnings")));
    TestEqual(TEXT("the root's body is NoCollision"),
        Smc->BodyInstance.GetCollisionEnabled(/*bCheckOwner=*/false), ECollisionEnabled::Type(ECollisionEnabled::NoCollision));

    MeshActor->SetActorEnableCollision(false);
    const FOutcome Enable = SetCollision(MeshActor->GetName(), true);
    TestTrue(TEXT("enable on a primitive root succeeds"), Enable.bSuccess);
    TestEqual(TEXT("the root's own body setting was written"),
        Smc->BodyInstance.GetCollisionEnabled(/*bCheckOwner=*/false), ECollisionEnabled::Type(ECollisionEnabled::QueryAndPhysics));
    bReported = true;
    TestTrue(TEXT("reply carries collisionEnabled"),
        Enable.Result.IsValid() && Enable.Result->TryGetBoolField(TEXT("collisionEnabled"), bReported));
    TestFalse(TEXT("reply reports the read-back (actor-level collision off), not the request"), bReported);
    bool bActorCollision = true;
    TestTrue(TEXT("reply carries actorEnableCollision"),
        Enable.Result.IsValid() && Enable.Result->TryGetBoolField(TEXT("actorEnableCollision"), bActorCollision));
    TestFalse(TEXT("reply reports actor-level collision off"), bActorCollision);
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestTrue(TEXT("a read-back that differs from the request carries a warning naming bActorEnableCollision"),
        Enable.Result.IsValid() && Enable.Result->TryGetArrayField(TEXT("warnings"), Warnings)
            && Warnings->Num() == 1 && (*Warnings)[0]->AsString().Contains(TEXT("bActorEnableCollision")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
