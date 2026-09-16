// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
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
    FString MakeScsDuplicateAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    void CleanupScsDuplicateAsset(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

        UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath);
        UObject* ExistingObject = ExistingPackage
            ? FindObject<UObject>(ExistingPackage, *AssetName)
            : FindObject<UObject>(nullptr, *ObjectPath);

        if (ExistingPackage && ExistingPackage->HasAnyFlags(RF_Transient))
        {
            FAssetRegistryModule::PackageDeleted(ExistingPackage);
            ExistingPackage->ClearFlags(RF_Standalone | RF_Public);
            ExistingPackage->SetDirtyFlag(false);

            if (ExistingObject)
            {
                ExistingObject->ClearFlags(RF_Standalone | RF_Public);
                ExistingObject->RemoveFromRoot();
                ExistingObject->MarkAsGarbage();
            }

            ExistingPackage->MarkAsGarbage();
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            return;
        }

        if (ExistingObject)
        {
            if (!ExistingObject->IsAsset())
            {
                ExistingPackage = ExistingObject->GetOutermost();
                if (!ExistingPackage)
                {
                    ExistingPackage = FindPackage(nullptr, *PackagePath);
                }
                if (ExistingPackage)
                {
                    FAssetRegistryModule::PackageDeleted(ExistingPackage);
                    ExistingPackage->ClearFlags(RF_Standalone | RF_Public);
                    ExistingPackage->SetDirtyFlag(false);
                    ExistingPackage->MarkAsGarbage();
                }

                ExistingObject->ClearFlags(RF_Standalone | RF_Public);
                ExistingObject->RemoveFromRoot();
                ExistingObject->MarkAsGarbage();

                CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
                return;
            }
        }

        // No force-delete tail. The only caller passes MakeScsDuplicateAssetPath(), which always
        // yields a /Game/__PW_GatewayTests/ path, so the guard that used to sit here
        // (`StartsWith("/Game/__PW_GatewayTests/") -> return`) always fired and the
        // UEditorAssetLibrary::DeleteAsset branches below it were unreachable.
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsDuplicateCopiesTemplateAttachmentAndChildrenTest,
    "PinWright.blueprint.scs.duplicate_component.CopiesTemplateAttachmentAndChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintScsDuplicateCopiesTemplateAttachmentAndChildrenTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsDuplicateAssetPath(TEXT("BP_DuplicateScs"));
    ON_SCOPE_EXIT { CleanupScsDuplicateAsset(BPPath); };

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
    USCS_Node* ParentRoot = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("ParentRoot"));
    TestNotNull(TEXT("parent root created"), ParentRoot);
    if (!ParentRoot || !ParentRoot->ComponentTemplate)
    {
        return false;
    }
    SCS->AddNode(ParentRoot);

    USCS_Node* SourceComponent = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("SourceChild"));
    TestNotNull(TEXT("source child created"), SourceComponent);
    if (!SourceComponent || !SourceComponent->ComponentTemplate)
    {
        return false;
    }
    ParentRoot->AddChildNode(SourceComponent);
    SourceComponent->AttachToName = FName(TEXT("ParentSocket"));

    USceneComponent* SourceTemplate = Cast<USceneComponent>(SourceComponent->ComponentTemplate);
    TestNotNull(TEXT("source child template"), SourceTemplate);
    if (!SourceTemplate)
    {
        return false;
    }
    SourceTemplate->SetRelativeLocation(FVector(10.0, 20.0, 30.0));
    SourceTemplate->SetRelativeRotation(FRotator(1.0, 2.0, 3.0));
    SourceTemplate->ComponentTags.Add(TEXT("CopiedTag"));
    SourceTemplate->SetVisibility(false);

    USCS_Node* SourceGrandchild = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("SourceGrandchild"));
    TestNotNull(TEXT("source grandchild created"), SourceGrandchild);
    if (!SourceGrandchild || !SourceGrandchild->ComponentTemplate)
    {
        return false;
    }
    SourceComponent->AddChildNode(SourceGrandchild);
    SourceGrandchild->AttachToName = FName(TEXT("ChildSocket"));
    USceneComponent* SourceGrandchildTemplate = Cast<USceneComponent>(SourceGrandchild->ComponentTemplate);
    TestNotNull(TEXT("source grandchild template"), SourceGrandchildTemplate);
    if (!SourceGrandchildTemplate)
    {
        return false;
    }
    SourceGrandchildTemplate->SetRelativeLocation(FVector(4.0, 5.0, 6.0));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), BPPath);
    Payload->SetStringField(TEXT("sourceName"), TEXT("SourceChild"));
    Payload->SetStringField(TEXT("newName"), TEXT("SourceChild_Copy"));
    Payload->SetBoolField(TEXT("duplicateChildren"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.duplicate_component handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.duplicate_component"), Payload, Capture));
    TestTrue(TEXT("blueprint.scs.duplicate_component succeeded"), Capture.bSuccess);

    USCS_Node* DuplicatedComponent = SCS->FindSCSNode(FName(TEXT("SourceChild_Copy")));
    TestNotNull(TEXT("duplicated child exists"), DuplicatedComponent);
    if (!DuplicatedComponent || !DuplicatedComponent->ComponentTemplate)
    {
        return false;
    }

    TestEqual(TEXT("duplicated child class copied"), DuplicatedComponent->ComponentClass, SourceComponent->ComponentClass);
    TestEqual(TEXT("duplicated child structural parent"), SCS->FindParentNode(DuplicatedComponent), ParentRoot);
    TestEqual(TEXT("duplicated child parent metadata remains root-only"), DuplicatedComponent->ParentComponentOrVariableName, FName(NAME_None));
    TestEqual(TEXT("duplicated child socket copied"), DuplicatedComponent->AttachToName, FName(TEXT("ParentSocket")));

    USceneComponent* DuplicatedTemplate = Cast<USceneComponent>(DuplicatedComponent->ComponentTemplate);
    TestNotNull(TEXT("duplicated child template is scene component"), DuplicatedTemplate);
    if (DuplicatedTemplate)
    {
        TestEqual(TEXT("relative location copied"), DuplicatedTemplate->GetRelativeLocation(), FVector(10.0, 20.0, 30.0));
        TestEqual(TEXT("relative rotation copied"), DuplicatedTemplate->GetRelativeRotation(), FRotator(1.0, 2.0, 3.0));
        TestTrue(TEXT("component tag copied"), DuplicatedTemplate->ComponentTags.Contains(TEXT("CopiedTag")));
        TestFalse(TEXT("visibility copied"), DuplicatedTemplate->IsVisible());
    }

    TestEqual(TEXT("duplicated grandchild attached"), (int32)DuplicatedComponent->GetChildNodes().Num(), 1);
    if (DuplicatedComponent->GetChildNodes().Num() == 1)
    {
        USCS_Node* DuplicatedGrandchild = DuplicatedComponent->GetChildNodes()[0];
        TestTrue(TEXT("duplicated grandchild class copied"), DuplicatedGrandchild && DuplicatedGrandchild->ComponentClass == SourceGrandchild->ComponentClass);
        TestEqual(TEXT("duplicated grandchild structural parent"), SCS->FindParentNode(DuplicatedGrandchild), DuplicatedComponent);
        if (DuplicatedGrandchild)
        {
            TestEqual(TEXT("duplicated grandchild parent metadata remains root-only"), DuplicatedGrandchild->ParentComponentOrVariableName, FName(NAME_None));
            TestEqual(TEXT("duplicated grandchild socket copied"), DuplicatedGrandchild->AttachToName, FName(TEXT("ChildSocket")));
        }
        USceneComponent* DuplicatedGrandchildTemplate = DuplicatedGrandchild ? Cast<USceneComponent>(DuplicatedGrandchild->ComponentTemplate) : nullptr;
        TestTrue(TEXT("grandchild relative transform copied"),
            DuplicatedGrandchildTemplate && DuplicatedGrandchildTemplate->GetRelativeLocation().Equals(FVector(4.0, 5.0, 6.0)));
    }

    TestTrue(TEXT("mapping returned"), Capture.Result.IsValid() && Capture.Result->HasTypedField<EJson::Object>(TEXT("mapping")));
    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject> Mapping = Capture.Result->GetObjectField(TEXT("mapping"));
        FString MappedName;
        Mapping->TryGetStringField(TEXT("SourceChild"), MappedName);
        TestEqual(TEXT("child mapping returned"), MappedName, FString(TEXT("SourceChild_Copy")));
    }

    return true;
}
