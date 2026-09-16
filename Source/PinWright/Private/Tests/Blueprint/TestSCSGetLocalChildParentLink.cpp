// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-scs-get-local-child-parent-link-missing.
//
// blueprint.scs.get must emit a `parent` field for locally-authored SCS children
// (those attached via USCS_Node::AddChildNode, whose ParentComponentOrVariableName
// stays None) so the readback is consistent with the parent's `child_count` and the
// downstream scs.txt emitter can reconstruct the nested tree. Before the fix, `parent`
// was emitted only from ParentComponentOrVariableName, leaving local children with no
// `parent` and rendering the tree flat. This test builds an all-local tree (the common
// "add root + attach children under it" pattern) and asserts each child carries a
// `parent` pointing at the structural root.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace
{
    FString MakeScsGetAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    void CleanupScsGetAsset(const FString& PackagePath)
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

    // Returns the `parent` field of the named component in the scs.get `components`
    // array, or an empty string if the component is missing or has no `parent`.
    FString FindEmittedParent(const TSharedPtr<FJsonObject>& Result, const FString& ComponentName)
    {
        if (!Result.IsValid())
        {
            return FString();
        }
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (!Result->TryGetArrayField(TEXT("components"), Components) || !Components)
        {
            return FString();
        }
        for (const TSharedPtr<FJsonValue>& Value : *Components)
        {
            const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Obj.IsValid())
            {
                continue;
            }
            FString Name;
            if (Obj->TryGetStringField(TEXT("name"), Name) && Name == ComponentName)
            {
                FString Parent;
                Obj->TryGetStringField(TEXT("parent"), Parent);
                return Parent;
            }
        }
        return FString();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsGetEmitsParentForLocalChildrenTest,
    "PinWright.blueprint.scs.get.EmitsParentForLocalChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintScsGetEmitsParentForLocalChildrenTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsGetAssetPath(TEXT("BP_ScsGetLocalTree"));
    ON_SCOPE_EXIT { CleanupScsGetAsset(BPPath); };

    const FString AssetName = FPackageName::GetLongPackageAssetName(BPPath);
    UPackage* Package = CreatePackage(*BPPath);
    TestNotNull(TEXT("package allocated"), Package);
    if (!Package)
    {
        return false;
    }
    Package->SetFlags(RF_Transient);

    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*AssetName),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("blueprint created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

    // Build an all-local tree: a root with two children, both attached only via
    // AddChildNode (exactly how the add_component / reparent_component write paths
    // attach to a local parent). None of the children get ParentComponentOrVariableName.
    USCS_Node* Root = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("BeaconBase"));
    TestNotNull(TEXT("root created"), Root);
    USCS_Node* ChildA = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("AlarmLight"));
    TestNotNull(TEXT("child A created"), ChildA);
    USCS_Node* ChildB = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("DetectionZone"));
    TestNotNull(TEXT("child B created"), ChildB);
    if (!Root || !ChildA || !ChildB)
    {
        return false;
    }
    SCS->AddNode(Root);
    Root->AddChildNode(ChildA);
    Root->AddChildNode(ChildB);

    // Precondition: the children carry NO ParentComponentOrVariableName — the exact
    // state that produced the bug (parent link lives only in Root->GetChildNodes()).
    TestEqual(TEXT("child A has no ParentComponentOrVariableName"),
        ChildA->ParentComponentOrVariableName, FName(NAME_None));
    TestEqual(TEXT("child B has no ParentComponentOrVariableName"),
        ChildB->ParentComponentOrVariableName, FName(NAME_None));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), BPPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.get handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.get"), Payload, Capture));
    TestTrue(TEXT("blueprint.scs.get succeeded"), Capture.bSuccess);

    // The fix: each local child must emit a `parent` derived from the parent's
    // GetChildNodes(), consistent with BeaconBase's child_count of 2. Reverting the
    // fix leaves these empty and fails the test.
    TestEqual(TEXT("AlarmLight parent emitted from top-down structure"),
        FindEmittedParent(Capture.Result, TEXT("AlarmLight")), FString(TEXT("BeaconBase")));
    TestEqual(TEXT("DetectionZone parent emitted from top-down structure"),
        FindEmittedParent(Capture.Result, TEXT("DetectionZone")), FString(TEXT("BeaconBase")));

    // The root itself remains a root: no parent emitted.
    TestEqual(TEXT("BeaconBase root has no parent"),
        FindEmittedParent(Capture.Result, TEXT("BeaconBase")), FString());

    return true;
}
