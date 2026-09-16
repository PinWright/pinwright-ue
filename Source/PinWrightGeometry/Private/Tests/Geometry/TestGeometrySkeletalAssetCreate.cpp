// Copyright (c) 2026 Alexander Penkin. MIT License.

// The skeletal creator may legitimately update a referenced skeleton when it adds a missing
// bone or fills an empty preview slot. It must not save or dirty an unchanged shared skeleton.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Handlers/Geometry/GeometrySkeletalAssetCreate.h"
#include "Utils/AssetUtils.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "ReferenceSkeleton.h"
#include "Tests/TestUtils.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    FString GeometrySkeletalAssetCreateTest_UniquePath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    USkeleton* GeometrySkeletalAssetCreateTest_MakeSkeleton(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        USkeleton* Skeleton = NewObject<USkeleton>(
            Package, FName(*AssetName), RF_Public | RF_Standalone);
        if (!Skeleton)
        {
            return nullptr;
        }

        FReferenceSkeletonModifier Modifier(Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
            FTransform::Identity, /*bAllowMultipleRoots=*/true);
        Skeleton->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Skeleton);
        return Skeleton;
    }

    UDynamicMesh* GeometrySkeletalAssetCreateTest_MakeTriangle()
    {
        UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
        if (!Mesh)
        {
            return nullptr;
        }

        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            const int32 A = EditMesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
            const int32 B = EditMesh.AppendVertex(FVector3d(1.0, 0.0, 0.0));
            const int32 C = EditMesh.AppendVertex(FVector3d(0.0, 1.0, 0.0));
            EditMesh.AppendTriangle(A, B, C);
        });
        return Mesh;
    }

    FSkeletalMeshCreateSpec GeometrySkeletalAssetCreateTest_Spec(
        const FString& AssetPath, USkeleton* Skeleton, bool bSave)
    {
        FSkeletalMeshCreateSpec Spec;
        Spec.AssetPath = AssetPath;
        Spec.Skeleton = Skeleton;
        Spec.SourcePath = TEXT("synthetic/unchanged-skeleton.pwmodel");
        Spec.bSave = bSave;
        return Spec;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalAssetCreateSkipsUnchangedSkeletonSaveTest,
    "PinWright.Geometry.AssetCreate.SkeletalSkipsUnchangedSkeletonSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalAssetCreateSkipsUnchangedSkeletonSaveTest::RunTest(const FString& Parameters)
{
    const FString SkeletonPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SK_Shared"));
    const FString FirstAssetPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SKM_First"));
    const FString SecondAssetPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SKM_Second"));
    const FString NoSaveAssetPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SKM_NoSave"));

    USkeleton* Skeleton = GeometrySkeletalAssetCreateTest_MakeSkeleton(SkeletonPath);
    UDynamicMesh* SourceMesh = GeometrySkeletalAssetCreateTest_MakeTriangle();
    if (!TestNotNull(TEXT("synthetic skeleton created"), Skeleton)
        || !TestNotNull(TEXT("synthetic triangle mesh created"), SourceMesh))
    {
        CleanupTestAsset(SkeletonPath);
        return true;
    }

    Skeleton->AddToRoot();
    SourceMesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        SourceMesh->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(NoSaveAssetPath);
        CleanupTestAsset(SecondAssetPath);
        CleanupTestAsset(FirstAssetPath);
        CleanupTestAsset(SkeletonPath);
    };

    const FSkeletalMeshCreateResult First = CreateSkeletalMesh(
        SourceMesh, GeometrySkeletalAssetCreateTest_Spec(FirstAssetPath, Skeleton, /*bSave=*/true));
    if (!TestTrue(TEXT("the first create succeeds"), First.bSuccess)
        || !TestEqual(TEXT("the first create saves the newly changed skeleton"),
            First.SkeletonSaveState, EAssetSaveState::Written))
    {
        return true;
    }

    FString SkeletonFilename;
    if (!TestTrue(TEXT("the skeleton package resolves to a filename"),
            FPackageName::TryConvertLongPackageNameToFilename(
                SkeletonPath, SkeletonFilename, FPackageName::GetAssetPackageExtension())))
    {
        return true;
    }

    TArray<uint8> BytesBefore;
    TestTrue(TEXT("the first skeleton save produced a file"),
        FFileHelper::LoadFileToArray(BytesBefore, *SkeletonFilename));
    const FDateTime TimestampBefore = IFileManager::Get().GetTimeStamp(*SkeletonFilename);

    const FSkeletalMeshCreateResult Second = CreateSkeletalMesh(
        SourceMesh, GeometrySkeletalAssetCreateTest_Spec(SecondAssetPath, Skeleton, /*bSave=*/true));
    TestTrue(TEXT("the second create succeeds"), Second.bSuccess);
    TestEqual(TEXT("an unchanged skeleton is not requested for saving"),
        Second.SkeletonSaveState, EAssetSaveState::NotRequested);
    TestFalse(TEXT("the unchanged skeleton package remains clean"),
        Skeleton->GetOutermost()->IsDirty());

    TArray<uint8> BytesAfter;
    TestTrue(TEXT("the skeleton file remains readable"),
        FFileHelper::LoadFileToArray(BytesAfter, *SkeletonFilename));
    TestTrue(TEXT("an unchanged skeleton keeps identical bytes"), BytesAfter == BytesBefore);
    TestEqual(TEXT("an unchanged skeleton keeps its file timestamp"),
        IFileManager::Get().GetTimeStamp(*SkeletonFilename), TimestampBefore);

    const FSkeletalMeshCreateResult NoSave = CreateSkeletalMesh(
        SourceMesh, GeometrySkeletalAssetCreateTest_Spec(NoSaveAssetPath, Skeleton, /*bSave=*/false));
    TestTrue(TEXT("the no-save create succeeds"), NoSave.bSuccess);
    TestEqual(TEXT("no-save still does not request an unchanged skeleton save"),
        NoSave.SkeletonSaveState, EAssetSaveState::NotRequested);
    TestFalse(TEXT("a no-save compile does not dirty an unchanged skeleton"),
        Skeleton->GetOutermost()->IsDirty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalAssetCreateSavesNewMaterialUsageTest,
    "PinWright.Geometry.AssetCreate.SkeletalSavesNewMaterialUsage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalAssetCreateSavesNewMaterialUsageTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ParentPath = FString::Printf(TEXT("/Game/PinWrightTests/MatUsageParent_%s"), *Suffix);
    const FString InstancePath = FString::Printf(TEXT("/Game/PinWrightTests/MatUsageInstance_%s"), *Suffix);
    const FString SkeletonPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SK_MatUsage"));
    const FString MeshPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SKM_MatUsage"));

    UPackage* ParentPackage = CreatePackage(*ParentPath);
    UMaterial* Parent = ParentPackage
        ? NewObject<UMaterial>(ParentPackage,
            FName(*FPackageName::GetLongPackageAssetName(ParentPath)), RF_Public | RF_Standalone)
        : nullptr;
    if (!TestNotNull(TEXT("synthetic parent material created"), Parent))
    {
        CleanupTestAsset(ParentPath);
        return true;
    }
    Parent->PostEditChange();
    FAssetRegistryModule::AssetCreated(Parent);

    UPackage* InstancePackage = CreatePackage(*InstancePath);
    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;
    UMaterialInstanceConstant* Instance = InstancePackage && Factory
        ? Cast<UMaterialInstanceConstant>(Factory->FactoryCreateNew(
            UMaterialInstanceConstant::StaticClass(), InstancePackage,
            FName(*FPackageName::GetLongPackageAssetName(InstancePath)),
            RF_Public | RF_Standalone, nullptr, GWarn))
        : nullptr;
    if (!TestNotNull(TEXT("synthetic material instance created"), Instance))
    {
        CleanupTestAsset(ParentPath);
        CleanupTestAsset(InstancePath);
        return true;
    }
    Instance->SetParentEditorOnly(Parent);
    Instance->PostEditChange();
    FAssetRegistryModule::AssetCreated(Instance);

    USkeleton* Skeleton = GeometrySkeletalAssetCreateTest_MakeSkeleton(SkeletonPath);
    UDynamicMesh* SourceMesh = GeometrySkeletalAssetCreateTest_MakeTriangle();
    if (!TestNotNull(TEXT("synthetic skeleton created"), Skeleton)
        || !TestNotNull(TEXT("synthetic triangle mesh created"), SourceMesh))
    {
        CleanupTestAsset(ParentPath);
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(SkeletonPath);
        return true;
    }

    Skeleton->AddToRoot();
    SourceMesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        SourceMesh->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(SkeletonPath);
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(ParentPath);
    };

    // WHICH OBJECT CARRIES THE USAGE, AND THEREFORE WHICH FILE MUST CHANGE.
    //
    // From UE 5.8 a material INSTANCE has its own overridable usage flags, so enabling skeletal
    // usage on the bound instance changes the instance and the instance is what has to be saved.
    // Before 5.8 the instance has no such flags at all - UMaterialInstance::CheckMaterialUsage
    // forwards to the base UMaterial (MaterialInstance.cpp:1704) - so the base material is the
    // object that changed and the one that has to reach disk. Same property either way: the
    // object that now carries the usage was persisted, not merely flipped in memory.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    const FString CarrierPath = InstancePath;
    const uint32 SkeletalUsageBit = 1u << MATUSAGE_SkeletalMesh;
    TestFalse(TEXT("the fixture starts without skeletal usage"),
        Instance->GetUsageByFlag(MATUSAGE_SkeletalMesh));
#else
    const FString CarrierPath = ParentPath;
    TestFalse(TEXT("the fixture starts without skeletal usage"),
        Parent->GetUsageByFlag(MATUSAGE_SkeletalMesh));
#endif
    TestTrue(TEXT("the parent material saves"),
        SaveAssetToDiskReportingPresence(Parent, /*bForce=*/true));
    TestTrue(TEXT("the instance saves before the compile"),
        SaveAssetToDiskReportingPresence(Instance, /*bForce=*/true));

    FString CarrierFilename;
    TestTrue(TEXT("the usage-carrying package resolves to a filename"),
        FPackageName::TryConvertLongPackageNameToFilename(
            CarrierPath, CarrierFilename, FPackageName::GetAssetPackageExtension()));
    TArray<uint8> BytesBefore;
    TestTrue(TEXT("the baseline usage-carrier file exists"),
        FFileHelper::LoadFileToArray(BytesBefore, *CarrierFilename));

    FSkeletalMeshCreateSpec Spec = GeometrySkeletalAssetCreateTest_Spec(
        MeshPath, Skeleton, /*bSave=*/true);
    Spec.MaterialSlots.Add(TEXT("Surface"));
    Spec.MaterialBindings.Add(TEXT("Surface"), InstancePath);
    const FSkeletalMeshCreateResult Result = CreateSkeletalMesh(SourceMesh, Spec);

    TestTrue(TEXT("the skeletal create succeeds"), Result.bSuccess);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    TestTrue(TEXT("the instance has the skeletal usage override in memory"),
        (Instance->BasePropertyOverrides.bOverride_UsageFlags & SkeletalUsageBit) != 0);
    TestTrue(TEXT("the instance has the skeletal usage bit in memory"),
        (Instance->BasePropertyOverrides.UsageFlags & SkeletalUsageBit) != 0);
#else
    TestTrue(TEXT("the base material has the skeletal usage bit in memory"),
        Parent->GetUsageByFlag(MATUSAGE_SkeletalMesh));
#endif

    TArray<uint8> BytesAfter;
    TestTrue(TEXT("the usage-carrier file remains readable"),
        FFileHelper::LoadFileToArray(BytesAfter, *CarrierFilename));
    TestTrue(TEXT("the material that now carries the usage was persisted"), BytesAfter != BytesBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalAssetTakeoverNamesUnmanagedStateTest,
    "PinWright.Geometry.AssetCreate.SkeletalTakeoverNamesUnmanagedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalAssetTakeoverNamesUnmanagedStateTest::RunTest(
    const FString& Parameters)
{
    const FString SkeletonPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SK_Takeover"));
    const FString MeshPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SKM_Takeover"));
    USkeleton* Skeleton = GeometrySkeletalAssetCreateTest_MakeSkeleton(SkeletonPath);
    UDynamicMesh* SourceMesh = GeometrySkeletalAssetCreateTest_MakeTriangle();
    if (!TestNotNull(TEXT("synthetic skeleton for takeover created"), Skeleton)
        || !TestNotNull(TEXT("synthetic triangle for takeover created"), SourceMesh))
    {
        CleanupTestAsset(SkeletonPath);
        return true;
    }

    Skeleton->AddToRoot();
    SourceMesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        SourceMesh->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(SkeletonPath);
    };

    FSkeletalMeshCreateSpec OriginalSpec = GeometrySkeletalAssetCreateTest_Spec(
        MeshPath, Skeleton, /*bSave=*/false);
    OriginalSpec.SourcePath = TEXT("Tests/Owners/original-skeletal.pwmodel");
    const FSkeletalMeshCreateResult First = CreateSkeletalMesh(SourceMesh, OriginalSpec);
    if (!TestTrue(TEXT("the original skeletal source creates the asset"), First.bSuccess)
        || !TestNotNull(TEXT("the original skeletal mesh is available"), First.Asset))
    {
        return true;
    }

    USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(First.Asset);
    Socket->SocketName = FName(TEXT("Attachment"));
    Socket->BoneName = FName(TEXT("root"));
    First.Asset->GetMeshOnlySocketList().Add(Socket);

    FSkeletalMeshCreateSpec TakeoverSpec = GeometrySkeletalAssetCreateTest_Spec(
        MeshPath, Skeleton, /*bSave=*/false);
    TakeoverSpec.SourcePath = TEXT("Tests/Owners/replacement-skeletal.pwmodel");
    const FSkeletalMeshCreateResult Refused = CreateSkeletalMesh(SourceMesh, TakeoverSpec);
    TestFalse(TEXT("a different skeletal source needs takeover permission"), Refused.bSuccess);
    const FPwDiagnostic* RefusalDiagnostic = Refused.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    TestNotNull(TEXT("the skeletal takeover refusal carries the guard code"),
        RefusalDiagnostic);
    if (RefusalDiagnostic)
    {
        TestEqual(TEXT("the blocked skeletal takeover is an error"),
            RefusalDiagnostic->Severity, EPwSeverity::Error);
        TestTrue(TEXT("the skeletal takeover refusal names the socket"),
            RefusalDiagnostic->Message.Contains(TEXT("socket[Attachment]")));
        TestTrue(TEXT("the skeletal ownership refusal includes the state that would be lost"),
            Refused.ErrorMessage.Contains(TEXT("socket[Attachment]")));
    }

    TakeoverSpec.bOverwrite = true;
    const FSkeletalMeshCreateResult Allowed = CreateSkeletalMesh(SourceMesh, TakeoverSpec);
    TestTrue(TEXT("overwrite=true permits the skeletal takeover"), Allowed.bSuccess);
    const FPwDiagnostic* WarningDiagnostic = Allowed.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    TestNotNull(TEXT("the permitted skeletal takeover keeps the guard code visible"),
        WarningDiagnostic);
    if (WarningDiagnostic)
    {
        TestEqual(TEXT("the permitted skeletal takeover is a warning"),
            WarningDiagnostic->Severity, EPwSeverity::Warning);
    }
    return true;
}

// The skeletal twin of ReconciledMaterialPathIsNotUnmanagedState. The live side of a material
// slot is read with GetPathName() - the OBJECT path /Pkg.Object - while a .pwmodel binds a
// material by the PACKAGE path /Pkg, and no source spelling produces the object form. Comparing
// the two raw made the guard name a material as state that would be lost even when the incoming
// source bound that exact material, so the remedy it prints could not be carried out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalAssetMaterialPathShapeTest,
    "PinWright.Geometry.AssetCreate.SkeletalReconciledMaterialPathIsNotUnmanagedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalAssetMaterialPathShapeTest::RunTest(const FString& Parameters)
{
    const FString SkeletonPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SK_MatPath"));
    const FString MeshPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("SKM_MatPath"));
    const FString MaterialPath = GeometrySkeletalAssetCreateTest_UniquePath(TEXT("M_MatPath"));

    UPackage* MaterialPackage = CreatePackage(*MaterialPath);
    UMaterial* Material = MaterialPackage
        ? NewObject<UMaterial>(MaterialPackage,
            FName(*FPackageName::GetLongPackageAssetName(MaterialPath)), RF_Public | RF_Standalone)
        : nullptr;
    if (!TestNotNull(TEXT("synthetic skeletal material created"), Material))
    {
        CleanupTestAsset(MaterialPath);
        return true;
    }
    // Pre-enabled so the create path never has to flip the usage and persist the material: this
    // test is about the recompile guard, not about usage propagation.
    //
    // UMaterial::SetUsageByFlag became public in 5.8, the release that also deprecated the
    // `bUsedWith*` UPROPERTY spelling; before 5.8 the setter is private, so the property write is
    // the only route. Both write the same member and neither recompiles.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
#else
    Material->bUsedWithSkeletalMesh = true;
#endif
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    USkeleton* Skeleton = GeometrySkeletalAssetCreateTest_MakeSkeleton(SkeletonPath);
    UDynamicMesh* SourceMesh = GeometrySkeletalAssetCreateTest_MakeTriangle();
    if (!TestNotNull(TEXT("synthetic skeleton for the material path check created"), Skeleton)
        || !TestNotNull(TEXT("synthetic triangle for the material path check created"), SourceMesh))
    {
        CleanupTestAsset(MaterialPath);
        CleanupTestAsset(SkeletonPath);
        return true;
    }

    Skeleton->AddToRoot();
    SourceMesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        SourceMesh->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(SkeletonPath);
        CleanupTestAsset(MaterialPath);
    };

    // The source binds the material the only way a source can: by package path.
    FSkeletalMeshCreateSpec OriginalSpec = GeometrySkeletalAssetCreateTest_Spec(
        MeshPath, Skeleton, /*bSave=*/false);
    OriginalSpec.SourcePath = TEXT("Tests/Owners/original-material-path.pwmodel");
    OriginalSpec.MaterialSlots.Add(TEXT("Surface"));
    OriginalSpec.MaterialBindings.Add(TEXT("Surface"), MaterialPath);
    const FSkeletalMeshCreateResult First = CreateSkeletalMesh(SourceMesh, OriginalSpec);
    if (!TestTrue(TEXT("the original skeletal source creates the asset"), First.bSuccess)
        || !TestNotNull(TEXT("the original skeletal mesh is available"), First.Asset))
    {
        return true;
    }

    // A takeover compares live state directly against the incoming source, with no baseline to
    // fall back on - so this is the comparison the shape mismatch corrupts. The incoming source
    // binds the SAME material, so the material is not state anyone would lose.
    FSkeletalMeshCreateSpec SameMaterialTakeover = GeometrySkeletalAssetCreateTest_Spec(
        MeshPath, Skeleton, /*bSave=*/false);
    SameMaterialTakeover.SourcePath = TEXT("Tests/Owners/replacement-material-path.pwmodel");
    SameMaterialTakeover.MaterialSlots.Add(TEXT("Surface"));
    SameMaterialTakeover.MaterialBindings.Add(TEXT("Surface"), MaterialPath);
    const FSkeletalMeshCreateResult SameMaterial =
        CreateSkeletalMesh(SourceMesh, SameMaterialTakeover);
    TestFalse(TEXT("a takeover still needs permission for the asset itself"),
        SameMaterial.bSuccess);
    TestFalse(TEXT("a package path and an object path naming ONE material are not a loss"),
        SameMaterial.ErrorMessage.Contains(TEXT("material[Surface]")));
    const FPwDiagnostic* SameMaterialDiagnostic = SameMaterial.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    if (SameMaterialDiagnostic)
    {
        TestFalse(TEXT("the guard does not name the material it is about to rebind identically"),
            SameMaterialDiagnostic->Message.Contains(TEXT("material[Surface]")));
    }

    // The other direction, on the same asset: a source that declares no material really would
    // discard the live one, and the guard has to keep saying so.
    FSkeletalMeshCreateSpec NoMaterialTakeover = GeometrySkeletalAssetCreateTest_Spec(
        MeshPath, Skeleton, /*bSave=*/false);
    NoMaterialTakeover.SourcePath = TEXT("Tests/Owners/replacement-material-path.pwmodel");
    const FSkeletalMeshCreateResult NoMaterial =
        CreateSkeletalMesh(SourceMesh, NoMaterialTakeover);
    TestFalse(TEXT("a takeover that drops the material is refused"), NoMaterial.bSuccess);
    const FPwDiagnostic* NoMaterialDiagnostic = NoMaterial.Diagnostics.FindByPredicate(
        [](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE;
        });
    if (TestNotNull(TEXT("dropping the material carries the guard code"), NoMaterialDiagnostic))
    {
        TestTrue(TEXT("the refusal names the material the source would discard"),
            NoMaterialDiagnostic->Message.Contains(TEXT("material[Surface]")));
    }
    return true;
}
