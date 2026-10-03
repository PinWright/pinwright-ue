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
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
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
        return FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/%s_%s"),
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

// B-scs-get-inherited-override-rows-no-parent: an ICH override row must carry the
// parent its overridden ancestor node hangs from, and every row's children/child_count
// must agree with the parent links the response emits, whichever producer emitted them.
// Shape mirrors BP_Weapon_AR: WeaponRoot (parent BP) owns WeaponMesh (parent BP,
// overridden in the child via ICH) and MagazineMesh (child BP's own SCS node).
namespace TestSCSGetInheritedOverrideHelpers
{
    UBlueprint* CreateBlueprintAt(const FString& PackagePath, UClass* ParentClass)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);
        return FKismetEditorUtilities::CreateBlueprint(ParentClass, Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    }

    TSharedPtr<FJsonObject> FindRow(const TSharedPtr<FJsonObject>& Result, const FString& Name)
    {
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("components"), Components) || !Components)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Components)
        {
            const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            FString RowName;
            if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), RowName) && RowName == Name)
            {
                return Obj;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsGetInheritedOverrideHierarchyTest,
    "PinWright.blueprint.scs.get.InheritedOverrideRowsCarryParentAndChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintScsGetInheritedOverrideHierarchyTest::RunTest(const FString& Parameters)
{
    using namespace TestSCSGetInheritedOverrideHelpers;

    const FString ParentPath = MakeScsGetAssetPath(TEXT("BP_ScsGetIchParent"));
    const FString ChildPath = MakeScsGetAssetPath(TEXT("BP_ScsGetIchChild"));
    ON_SCOPE_EXIT { CleanupScsGetAsset(ChildPath); CleanupScsGetAsset(ParentPath); };

    UBlueprint* ParentBP = CreateBlueprintAt(ParentPath, AActor::StaticClass());
    if (!TestNotNull(TEXT("parent blueprint created"), ParentBP) || !ParentBP->SimpleConstructionScript)
    {
        return false;
    }
    USimpleConstructionScript* ParentSCS = ParentBP->SimpleConstructionScript;
    USCS_Node* WeaponRoot = ParentSCS->CreateNode(USceneComponent::StaticClass(), TEXT("WeaponRoot"));
    USCS_Node* WeaponMesh = ParentSCS->CreateNode(USceneComponent::StaticClass(), TEXT("WeaponMesh"));
    if (!TestNotNull(TEXT("WeaponRoot created"), WeaponRoot) || !TestNotNull(TEXT("WeaponMesh created"), WeaponMesh))
    {
        return false;
    }
    ParentSCS->AddNode(WeaponRoot);
    WeaponRoot->AddChildNode(WeaponMesh);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ParentBP);
    FKismetEditorUtilities::CompileBlueprint(ParentBP);
    WeaponRoot = ParentSCS->FindSCSNode(TEXT("WeaponRoot"));
    WeaponMesh = ParentSCS->FindSCSNode(TEXT("WeaponMesh"));
    if (!TestNotNull(TEXT("WeaponRoot survived compile"), WeaponRoot)
        || !TestNotNull(TEXT("WeaponMesh survived compile"), WeaponMesh)
        || !TestNotNull(TEXT("parent generated class"), ParentBP->GeneratedClass.Get()))
    {
        return false;
    }

    UBlueprint* ChildBP = CreateBlueprintAt(ChildPath, ParentBP->GeneratedClass);
    if (!TestNotNull(TEXT("child blueprint created"), ChildBP) || !ChildBP->SimpleConstructionScript)
    {
        return false;
    }
    UInheritableComponentHandler* ICH = ChildBP->GetInheritableComponentHandler(true);
    USceneComponent* Override = ICH
        ? Cast<USceneComponent>(ICH->CreateOverridenComponentTemplate(FComponentKey(WeaponMesh)))
        : nullptr;
    if (!TestNotNull(TEXT("ICH override for WeaponMesh created"), Override))
    {
        return false;
    }
    Override->SetRelativeLocation_Direct(FVector(10.0, 0.0, 0.0));

    USCS_Node* MagazineMesh = ChildBP->SimpleConstructionScript->CreateNode(
        USceneComponent::StaticClass(), TEXT("MagazineMesh"));
    if (!TestNotNull(TEXT("MagazineMesh created"), MagazineMesh))
    {
        return false;
    }
    ChildBP->SimpleConstructionScript->AddNode(MagazineMesh);
    MagazineMesh->SetParent(WeaponRoot);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ChildBP);
    FKismetEditorUtilities::CompileBlueprint(ChildBP);

    // Preconditions: WeaponMesh is reachable only as an ICH record (not a local node),
    // and its attachment lives only in the parent BP's WeaponRoot->GetChildNodes().
    ICH = ChildBP->GetInheritableComponentHandler(false);
    TestTrue(TEXT("ICH record for WeaponMesh survived the child compile"),
        ICH && ICH->GetOverridenComponentTemplate(FComponentKey(WeaponMesh)) != nullptr);
    TestNull(TEXT("child SCS does not own WeaponMesh"),
        ChildBP->SimpleConstructionScript->FindSCSNode(TEXT("WeaponMesh")));
    TestEqual(TEXT("WeaponMesh has no ParentComponentOrVariableName"),
        WeaponMesh->ParentComponentOrVariableName, FName(NAME_None));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ChildPath);
    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.get handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.get"), Payload, Capture));
    if (!TestTrue(TEXT("blueprint.scs.get succeeded"), Capture.bSuccess))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> MeshRow = FindRow(Capture.Result, TEXT("WeaponMesh"));
    const TSharedPtr<FJsonObject> RootRow = FindRow(Capture.Result, TEXT("WeaponRoot"));
    if (!TestTrue(TEXT("WeaponMesh row emitted"), MeshRow.IsValid())
        || !TestTrue(TEXT("WeaponRoot row emitted"), RootRow.IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("WeaponMesh row comes from the ICH producer"),
        MeshRow->GetStringField(TEXT("source")), FString(TEXT("inherited-override")));
    TestEqual(TEXT("inherited-override row carries its parent"),
        FindEmittedParent(Capture.Result, TEXT("WeaponMesh")), FString(TEXT("WeaponRoot")));
    TestEqual(TEXT("local child of the inherited root carries its parent"),
        FindEmittedParent(Capture.Result, TEXT("MagazineMesh")), FString(TEXT("WeaponRoot")));

    TSet<FString> RootChildren;
    const TArray<TSharedPtr<FJsonValue>>* Kids = nullptr;
    if (TestTrue(TEXT("WeaponRoot emits children[]"), RootRow->TryGetArrayField(TEXT("children"), Kids) && Kids))
    {
        for (const TSharedPtr<FJsonValue>& Kid : *Kids)
        {
            RootChildren.Add(Kid->AsString());
        }
    }
    TestTrue(TEXT("WeaponRoot children[] lists the ICH-overridden WeaponMesh"), RootChildren.Contains(TEXT("WeaponMesh")));
    TestTrue(TEXT("WeaponRoot children[] lists the local MagazineMesh"), RootChildren.Contains(TEXT("MagazineMesh")));
    TestEqual(TEXT("WeaponRoot child_count counts children from both producers"),
        static_cast<int32>(RootRow->GetNumberField(TEXT("child_count"))), 2);

    // Invariant on every row: child_count == children.Num(), and each listed child
    // points back at this row via its own `parent`.
    const TArray<TSharedPtr<FJsonValue>>& Rows = Capture.Result->GetArrayField(TEXT("components"));
    for (const TSharedPtr<FJsonValue>& Value : Rows)
    {
        const TSharedPtr<FJsonObject> Row = Value->AsObject();
        const FString Name = Row->GetStringField(TEXT("name"));
        const TArray<TSharedPtr<FJsonValue>>* RowKids = nullptr;
        if (!TestTrue(FString::Printf(TEXT("%s emits children[]"), *Name),
            Row->TryGetArrayField(TEXT("children"), RowKids) && RowKids))
        {
            continue;
        }
        TestEqual(FString::Printf(TEXT("%s child_count matches children[]"), *Name),
            static_cast<int32>(Row->GetNumberField(TEXT("child_count"))), RowKids->Num());
        for (const TSharedPtr<FJsonValue>& Kid : *RowKids)
        {
            TestEqual(FString::Printf(TEXT("%s's child %s points back via parent"), *Name, *Kid->AsString()),
                FindEmittedParent(Capture.Result, Kid->AsString()), Name);
        }
    }
    return true;
}
