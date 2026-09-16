// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-scs-set-property-bodyinstance-collision-silent-noop:
// blueprint.scs.set_property on BodyInstance.CollisionEnabled /
// BodyInstance.CollisionProfileName returned success while every SPAWNED instance of the
// Blueprint kept QueryAndPhysics.
//
// GROUND TRUTH IS THE SPAWNED COMPONENT, NOT THE TEMPLATE, and that distinction is the whole
// point. The pre-fix store landed on the template and read back correctly off the template -
// a read-after-write against the object the write touched passes on the broken build. It is
// the instance that disagrees, because instancing re-derives the collision fields from
// CollisionProfileName AFTER the archetype values arrive:
//   USCS_Node::ExecuteNodeOnActor -> AActor::CreateComponentFromTemplate
//     -> StaticDuplicateObjectEx, whose FlagMask strips RF_ArchetypeObject
//     -> ConditionalPostLoad (run for exactly the non-template duplicates)
//     -> UPrimitiveComponent::PostLoad -> FBodyInstance::FixupData (guard: !IsTemplate())
//     -> LoadProfileData -> UCollisionProfile::ReadConfig, which assigns CollisionEnabled,
//        ObjectType and the response container straight off the profile.
// Every UPrimitiveComponent constructor installs the BlockAll profile, so what gets
// re-applied is QueryAndPhysics. These tests therefore spawn the class and read the
// component the physics scene would consult.
//
// The profile test asserts on BOTH the template and the instance, and needs to: a raw store
// of CollisionProfileName = "NoCollision" leaves the TEMPLATE claiming a profile it does not
// implement (name written, LoadProfileData never run), while the instance is repaired by the
// FixupData above and looks right. Only the template assertion separates the two.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace
{
    // Every helper here carries a file-unique prefix: the plugin builds with Unity on, so an
    // anonymous-namespace name shared with a sibling Tests/Blueprint file would ODR-clash.
    const TCHAR* const ScsCollisionProbeComponentName = TEXT("Body");

    // Far from the open map's content so a probe that somehow outlives the guard is not
    // sitting on top of another fixture.
    const FVector ScsCollisionProbeOrigin(0.0, 0.0, 16000.0);

    FString MakeScsCollisionAssetPath()
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/BP_ScsSetPropCollision_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    void CleanupScsCollisionAsset(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath);
        UObject* ExistingObject = ExistingPackage
            ? FindObject<UObject>(ExistingPackage, *AssetName)
            : nullptr;

        if (ExistingPackage)
        {
            FAssetRegistryModule::PackageDeleted(ExistingPackage);
            ExistingPackage->ClearFlags(RF_Standalone | RF_Public);
            ExistingPackage->SetDirtyFlag(false);
            ExistingPackage->MarkAsGarbage();
        }
        if (ExistingObject)
        {
            ExistingObject->ClearFlags(RF_Standalone | RF_Public);
            ExistingObject->RemoveFromRoot();
            ExistingObject->MarkAsGarbage();
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    // A transient Blueprint whose SCS holds one static-mesh component template — the state
    // blueprint.scs.add_component leaves behind, built directly so the fixture cannot fail
    // for a reason belonging to another verb.
    UBlueprint* MakeScsCollisionFixture(FAutomationTestBase& Test, const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            Test.AddError(TEXT("fixture package could not be allocated"));
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!Blueprint || !Blueprint->SimpleConstructionScript)
        {
            Test.AddError(TEXT("fixture blueprint could not be created"));
            return nullptr;
        }

        USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
        USCS_Node* Node = SCS->CreateNode(UStaticMeshComponent::StaticClass(),
                                          ScsCollisionProbeComponentName);
        if (!Node)
        {
            Test.AddError(TEXT("fixture SCS node could not be created"));
            return nullptr;
        }
        SCS->AddNode(Node);
        return Blueprint;
    }

    UStaticMeshComponent* FindScsCollisionTemplate(UBlueprint* Blueprint)
    {
        if (!Blueprint || !Blueprint->SimpleConstructionScript)
        {
            return nullptr;
        }
        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node && Node->GetVariableName().IsValid() &&
                Node->GetVariableName().ToString().Equals(ScsCollisionProbeComponentName,
                                                          ESearchCase::IgnoreCase))
            {
                return Cast<UStaticMeshComponent>(Node->ComponentTemplate);
            }
        }
        return nullptr;
    }

    // Dispatches blueprint.scs.set_property and returns the captured response.
    bool RunScsCollisionSetProperty(FAutomationTestBase& Test, const FString& PackagePath,
                                    const FString& PropertyName,
                                    const TSharedPtr<FJsonValue>& PropertyValue,
                                    FTestResponseCapture& OutCapture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetStringField(TEXT("componentName"), ScsCollisionProbeComponentName);
        Payload->SetStringField(TEXT("propertyName"), PropertyName);
        Payload->SetField(TEXT("propertyValue"), PropertyValue);

        if (!Test.TestTrue(TEXT("blueprint.scs.set_property handler found"),
                InvokeHandlerWithCapture(TEXT("blueprint.scs.set_property"), Payload, OutCapture)))
        {
            return false;
        }
        Test.TestTrue(TEXT("set_property succeeded"), OutCapture.bSuccess);
        return OutCapture.bSuccess;
    }

    // Spawns the compiled class into the editor world and hands back the SCS-created static
    // mesh component ON THE INSTANCE — the object the ticket's read-back looked at. Spawned
    // RF_Transient because nothing here drives an actor.* verb (which would filter it out);
    // the caller's FScopedEditorWorldActorGuard destroys it either way.
    UStaticMeshComponent* SpawnScsCollisionInstanceComponent(FAutomationTestBase& Test,
                                                             UBlueprint* Blueprint)
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World || !Blueprint || !Blueprint->GeneratedClass)
        {
            Test.AddError(TEXT("no editor world or no compiled class to spawn"));
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Spawned = World->SpawnActor<AActor>(Blueprint->GeneratedClass,
            ScsCollisionProbeOrigin, FRotator::ZeroRotator, SpawnParams);
        if (!Spawned)
        {
            Test.AddError(TEXT("fixture class did not spawn"));
            return nullptr;
        }

        TInlineComponentArray<UStaticMeshComponent*> Components;
        Spawned->GetComponents(Components);
        if (Components.Num() == 0)
        {
            Test.AddError(TEXT("spawned actor carries no static mesh component"));
            return nullptr;
        }
        return Components[0];
    }

    FString ScsCollisionEnabledName(ECollisionEnabled::Type Value)
    {
        switch (Value)
        {
        case ECollisionEnabled::NoCollision:     return TEXT("NoCollision");
        case ECollisionEnabled::QueryOnly:       return TEXT("QueryOnly");
        case ECollisionEnabled::PhysicsOnly:     return TEXT("PhysicsOnly");
        case ECollisionEnabled::QueryAndPhysics: return TEXT("QueryAndPhysics");
        default:                                 return TEXT("Unknown");
        }
    }
}

// ============================================================================
// CollisionEnabled = NoCollision on an SCS template must be what the SPAWNED component
// reports. The pre-fix raw store left CollisionProfileName at BlockAll, and FixupData put
// QueryAndPhysics back on every instance.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsSetPropertyCollisionEnabledReachesInstanceTest,
    "PinWright.blueprint.scs.set_property.CollisionEnabledReachesTheSpawnedInstance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsSetPropertyCollisionEnabledReachesInstanceTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsCollisionAssetPath();
    ON_SCOPE_EXIT { CleanupScsCollisionAsset(BPPath); };
    FScopedEditorWorldActorGuard WorldGuard;

    UBlueprint* Blueprint = MakeScsCollisionFixture(*this, BPPath);
    if (!Blueprint)
    {
        return false;
    }

    // The fixture must START blocking, or a green result cannot distinguish "the write
    // landed" from "there was nothing to change".
    UStaticMeshComponent* Template = FindScsCollisionTemplate(Blueprint);
    if (!TestNotNull(TEXT("component template resolved"), Template))
    {
        return false;
    }
    TestEqual(TEXT("fixture template starts at QueryAndPhysics"),
        ScsCollisionEnabledName(Template->BodyInstance.GetCollisionEnabled()),
        FString(TEXT("QueryAndPhysics")));

    FTestResponseCapture Capture;
    if (!RunScsCollisionSetProperty(*this, BPPath, TEXT("BodyInstance.CollisionEnabled"),
            MakeShared<FJsonValueString>(TEXT("NoCollision")), Capture))
    {
        return false;
    }

    // The response has to be distinguishable from the raw-store response it used to be.
    bool bCollisionRouted = false;
    TestTrue(TEXT("response reports collisionRouted"),
        Capture.Result.IsValid() &&
        Capture.Result->TryGetBoolField(TEXT("collisionRouted"), bCollisionRouted));
    TestTrue(TEXT("collisionRouted is true"), bCollisionRouted);

    // Re-resolve: the handler compiles the Blueprint between the call and this read.
    Template = FindScsCollisionTemplate(Blueprint);
    if (!TestNotNull(TEXT("component template still resolves after compile"), Template))
    {
        return false;
    }
    TestEqual(TEXT("template reports NoCollision"),
        ScsCollisionEnabledName(Template->BodyInstance.GetCollisionEnabled()),
        FString(TEXT("NoCollision")));
    // The invariant that makes it survive instancing: an explicit CollisionEnabled is no
    // longer describable by the old profile, so the profile must have moved to Custom.
    // Leaving it at BlockAll is exactly what let LoadProfileData undo the write.
    TestEqual(TEXT("profile name was invalidated to Custom"),
        Template->BodyInstance.GetCollisionProfileName(),
        UCollisionProfile::CustomCollisionProfileName);

    // GROUND TRUTH.
    UStaticMeshComponent* Instance = SpawnScsCollisionInstanceComponent(*this, Blueprint);
    if (!Instance)
    {
        return false;
    }
    TestEqual(TEXT("the SPAWNED component reports NoCollision"),
        ScsCollisionEnabledName(Instance->GetCollisionEnabled()),
        FString(TEXT("NoCollision")));
    return true;
}

// ============================================================================
// CollisionProfileName = NoCollision must actually LOAD the profile onto the template, not
// just store the name. The template assertion is the load-bearing one here — see the file
// header for why the instance repairs itself either way.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsSetPropertyCollisionProfileLoadsProfileDataTest,
    "PinWright.blueprint.scs.set_property.CollisionProfileNameLoadsTheProfileData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsSetPropertyCollisionProfileLoadsProfileDataTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsCollisionAssetPath();
    ON_SCOPE_EXIT { CleanupScsCollisionAsset(BPPath); };
    FScopedEditorWorldActorGuard WorldGuard;

    UBlueprint* Blueprint = MakeScsCollisionFixture(*this, BPPath);
    if (!Blueprint)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!RunScsCollisionSetProperty(*this, BPPath, TEXT("BodyInstance.CollisionProfileName"),
            MakeShared<FJsonValueString>(TEXT("NoCollision")), Capture))
    {
        return false;
    }

    UStaticMeshComponent* Template = FindScsCollisionTemplate(Blueprint);
    if (!TestNotNull(TEXT("component template resolved"), Template))
    {
        return false;
    }
    TestEqual(TEXT("template names the requested profile"),
        Template->BodyInstance.GetCollisionProfileName(), FName(TEXT("NoCollision")));
    // A raw store writes the NAME and nothing else, leaving the template claiming a profile
    // it does not implement. Running the profile's data is what SetCollisionProfileName adds.
    TestEqual(TEXT("template implements the profile it names"),
        ScsCollisionEnabledName(Template->BodyInstance.GetCollisionEnabled()),
        FString(TEXT("NoCollision")));

    UStaticMeshComponent* Instance = SpawnScsCollisionInstanceComponent(*this, Blueprint);
    if (!Instance)
    {
        return false;
    }
    TestEqual(TEXT("the SPAWNED component reports NoCollision"),
        ScsCollisionEnabledName(Instance->GetCollisionEnabled()),
        FString(TEXT("NoCollision")));
    return true;
}

// ============================================================================
// A static-mesh component with bUseDefaultCollision re-reads its whole collision setup from
// the mesh at every registration, so a collision write on such a template is discarded on
// every spawn no matter which path applied it. The write has to clear the flag, the way the
// details panel does.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsSetPropertyCollisionClearsUseDefaultCollisionTest,
    "PinWright.blueprint.scs.set_property.CollisionWriteClearsUseDefaultCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsSetPropertyCollisionClearsUseDefaultCollisionTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!Cube)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("engine cube mesh unavailable; skipping bUseDefaultCollision test."));
        return true;
    }

    const FString BPPath = MakeScsCollisionAssetPath();
    ON_SCOPE_EXIT { CleanupScsCollisionAsset(BPPath); };
    FScopedEditorWorldActorGuard WorldGuard;

    UBlueprint* Blueprint = MakeScsCollisionFixture(*this, BPPath);
    if (!Blueprint)
    {
        return false;
    }

    // bUseDefaultCollision only bites when the component has a mesh whose body setup it can
    // borrow (UStaticMeshComponent::SupportsDefaultCollision), so the fixture needs both.
    UStaticMeshComponent* Template = FindScsCollisionTemplate(Blueprint);
    if (!TestNotNull(TEXT("component template resolved"), Template))
    {
        return false;
    }
    Template->SetStaticMesh(Cube);
    Template->bUseDefaultCollision = true;

    FTestResponseCapture Capture;
    if (!RunScsCollisionSetProperty(*this, BPPath, TEXT("BodyInstance.CollisionEnabled"),
            MakeShared<FJsonValueString>(TEXT("NoCollision")), Capture))
    {
        return false;
    }

    Template = FindScsCollisionTemplate(Blueprint);
    if (!TestNotNull(TEXT("component template still resolves after compile"), Template))
    {
        return false;
    }
    TestFalse(TEXT("an explicit collision write clears bUseDefaultCollision"),
        (bool)Template->bUseDefaultCollision);

    // GROUND TRUTH: with the flag still set, OnRegister would have restored the mesh's
    // collision onto this instance.
    UStaticMeshComponent* Instance = SpawnScsCollisionInstanceComponent(*this, Blueprint);
    if (!Instance)
    {
        return false;
    }
    TestEqual(TEXT("the SPAWNED component reports NoCollision"),
        ScsCollisionEnabledName(Instance->GetCollisionEnabled()),
        FString(TEXT("NoCollision")));
    return true;
}

// ============================================================================
// Narrowness guard: the routing must claim ONLY the collision fields. A non-collision
// BodyInstance sub-field keeps the plain reflection store and reports no collisionRouted.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsSetPropertyNonCollisionBodyFieldIsUnroutedTest,
    "PinWright.blueprint.scs.set_property.NonCollisionBodyFieldKeepsTheReflectionStore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsSetPropertyNonCollisionBodyFieldIsUnroutedTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsCollisionAssetPath();
    ON_SCOPE_EXIT { CleanupScsCollisionAsset(BPPath); };

    UBlueprint* Blueprint = MakeScsCollisionFixture(*this, BPPath);
    if (!Blueprint)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!RunScsCollisionSetProperty(*this, BPPath, TEXT("BodyInstance.MassScale"),
            MakeShared<FJsonValueNumber>(2.5), Capture))
    {
        return false;
    }

    bool bCollisionRouted = false;
    TestFalse(TEXT("a mass write is not reported as a collision route"),
        Capture.Result.IsValid() &&
        Capture.Result->TryGetBoolField(TEXT("collisionRouted"), bCollisionRouted));

    UStaticMeshComponent* Template = FindScsCollisionTemplate(Blueprint);
    if (!TestNotNull(TEXT("component template resolved"), Template))
    {
        return false;
    }
    TestEqual(TEXT("the mass write still landed on the template"),
        Template->BodyInstance.MassScale, 2.5f);
    // And it did not disturb the collision state on its way through.
    TestEqual(TEXT("collision is untouched by a non-collision write"),
        ScsCollisionEnabledName(Template->BodyInstance.GetCollisionEnabled()),
        FString(TEXT("QueryAndPhysics")));
    return true;
}
