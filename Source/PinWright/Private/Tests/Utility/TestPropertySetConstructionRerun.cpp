// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-property-set-wiki-construction-rerun.
//
// The property.set page used to say the verb "deliberately skips" the editor's
// PreEditChange/PostEditChange reregister pair, "that path flushes rendering commands and
// reruns construction scripts" - read by callers as "an actor write leaves construction-built
// components alone". Skipping PreEditChange avoids the rerun only for a COMPONENT target
// (ActorComponent.cpp:1437-1446). For a placed ACTOR target the notification the verb does
// emit is AActor::PostEditChangeProperty, which itself unregisters, reruns construction and
// re-registers (ActorEditor.cpp:170, :225/:240, gate ReregisterComponentsWhenModified :341).
//
// These tests measure both halves of what the page now states, so the page cannot drift from
// the engine again: an actor write destroys and rebuilds an SCS component, a component write
// does not, and the rendered method page says so.
#include "Misc/AutomationTest.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "Tests/Infra/WikiDocTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace TestPropertySetConstructionRerunHelpers
{
    const FName ProbeComponentName(TEXT("PWRerunProbe"));

    // Never-saved, transient Blueprint with one SCS scene component. The class is spawned
    // directly, so no asset-registry entry is needed and RF_Transient keeps it unsaveable.
    UBlueprint* CreateScsBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);
        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        if (!Blueprint || !Blueprint->SimpleConstructionScript)
        {
            return nullptr;
        }
        USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(
            USceneComponent::StaticClass(), ProbeComponentName);
        if (!Node)
        {
            return nullptr;
        }
        Blueprint->SimpleConstructionScript->AddNode(Node);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        return Blueprint;
    }

    void DiscardBlueprint(const FString& PackagePath)
    {
        UPackage* Package = FindPackage(nullptr, *PackagePath);
        if (!Package)
        {
            return;
        }
        if (UObject* Asset = FindObject<UObject>(Package,
                *FPackageName::GetLongPackageAssetName(PackagePath)))
        {
            Asset->ClearFlags(RF_Standalone | RF_Public);
            Asset->MarkAsGarbage();
        }
        Package->ClearFlags(RF_Standalone | RF_Public);
        Package->SetDirtyFlag(false);
        Package->MarkAsGarbage();
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    USceneComponent* FindProbe(AActor* Actor)
    {
        TInlineComponentArray<USceneComponent*> Components(Actor);
        for (USceneComponent* Component : Components)
        {
            if (IsValid(Component) && Component->GetFName() == ProbeComponentName)
            {
                return Component;
            }
        }
        return nullptr;
    }

    bool SetProperty(FAutomationTestBase& Test, UObject* Target, const TCHAR* PropertyName,
        const TSharedPtr<FJsonValue>& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Target->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), PropertyName);
        Payload->SetField(TEXT("value"), Value);
        Payload->SetBoolField(TEXT("markDirty"), false);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("property.set is registered"),
            InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("property.set %s succeeded (%s %s)"), PropertyName,
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        return Capture.bSuccess;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetActorTargetRerunsConstructionScriptTest,
    "PinWright.property.set.ActorTargetRerunsConstructionScript",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetActorTargetRerunsConstructionScriptTest::RunTest(const FString& Parameters)
{
    using namespace TestPropertySetConstructionRerunHelpers;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping property.set construction-rerun test."));
        return true;
    }

    const FString BPPath = FString::Printf(
        TEXT("/Game/PinWrightTests/__PW_GatewayTests/BP_PropSetRerun_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    // Declared before the guard so the guard's destructor removes the instance first.
    ON_SCOPE_EXIT { DiscardBlueprint(BPPath); };
    FScopedEditorWorldActorGuard WorldGuard;

    UBlueprint* Blueprint = CreateScsBlueprint(BPPath);
    TestNotNull(TEXT("fixture Blueprint with one SCS component"), Blueprint);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        return false;
    }
    AActor* Actor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, FTransform::Identity);
    TestNotNull(TEXT("fixture actor spawned in the editor world"), Actor);
    if (!Actor)
    {
        return false;
    }

    // Preconditions: a construction-built component exists, and the engine's own rerun gate
    // holds for this actor (not a template, not a PIE package, has a world).
    USceneComponent* Before = FindProbe(Actor);
    TestNotNull(TEXT("SCS probe component present on the instance"), Before);
    if (!Before)
    {
        return false;
    }
    TestTrue(TEXT("probe is construction-built (SCS)"),
        Before->CreationMethod == EComponentCreationMethod::SimpleConstructionScript);
    TestTrue(TEXT("ReregisterComponentsWhenModified holds for the placed actor"),
        Actor->ReregisterComponentsWhenModified());
    const TWeakObjectPtr<USceneComponent> BeforeWeak(Before);

    // ---- Component target: no rerun. The verb skips PreEditChange, so the component's
    // ConsolidatedPostEditChange finds no EditReregisterContexts entry and leaves the owner's
    // construction alone.
    if (!SetProperty(*this, Before, TEXT("bHiddenInGame"), MakeShared<FJsonValueBoolean>(true)))
    {
        return false;
    }
    TestTrue(TEXT("component-target write keeps the construction-built component alive"),
        BeforeWeak.IsValid());
    TestTrue(TEXT("component-target write leaves the same component on the actor"),
        FindProbe(Actor) == Before);

    // ---- Actor target: rerun. AActor::PostEditChangeProperty destroys and rebuilds every
    // construction-built component.
    if (!SetProperty(*this, Actor, TEXT("InitialLifeSpan"), MakeShared<FJsonValueNumber>(5.0)))
    {
        return false;
    }
    TestEqual(TEXT("actor write landed"), Actor->InitialLifeSpan, 5.0f);
    TestFalse(TEXT("actor-target write destroyed the pre-write construction-built component"),
        BeforeWeak.IsValid());
    USceneComponent* After = FindProbe(Actor);
    TestNotNull(TEXT("construction rebuilt the probe component"), After);
    TestTrue(TEXT("the rebuilt component is a different object"),
        After != nullptr && After != BeforeWeak.Get(/*bEvenIfGarbage=*/true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetConstructionRerunDocTest,
    "PinWright.infra.wiki_handler.MethodPage.PropertySetConstructionRerun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetConstructionRerunDocTest::RunTest(const FString& Parameters)
{
    FString Page;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("property.set"), Page))
    {
        return false;
    }
    TestTrue(TEXT("page states an actor-target write reruns construction"),
        Page.Contains(TEXT("An actor-target write reruns that actor's construction script")));
    TestTrue(TEXT("page states construction-built components are destroyed and rebuilt"),
        Page.Contains(TEXT("Every construction-built")) && Page.Contains(TEXT("is destroyed and rebuilt")));
    TestTrue(TEXT("page scopes the skipped reregister pair to a component target"),
        Page.Contains(TEXT("deliberately skips for a component")));
    TestFalse(TEXT("page no longer says the verb skips the rerun unconditionally"),
        Page.Contains(TEXT("verb deliberately skips — that path")));
    return true;
}
