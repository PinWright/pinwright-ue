// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometrySkeletalAssetCreate.cpp - see GeometrySkeletalAssetCreate.h for the shared seam.
#include "Handlers/Geometry/GeometrySkeletalAssetCreate.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryAssetCreate.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "PwSource/PwSourceRecompileGuard.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"

#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Package.h"
#include "UDynamicMesh.h"

#include "AssetUtils/CreateSkeletalMeshUtil.h"

namespace
{
    // Does this material interface declare the usage the caller needs?
    //
    // UMaterialInterface::GetUsageByFlag is 5.8-only: that is the release that gave a material
    // INSTANCE its own overridable usage flags. Before it, usage is a property of the base
    // UMaterial alone and an instance inherits it, so GetMaterial()->GetUsageByFlag asks the
    // same question of the object that actually answers it on those engines.
    bool GeometrySkeletalAssetCreate_HasUsage(UMaterialInterface* Material, EMaterialUsage Usage)
    {
        if (!Material)
        {
            return false;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        return Material->GetUsageByFlag(Usage);
#else
        const UMaterial* BaseMaterial = Material->GetMaterial();
        return BaseMaterial != nullptr && BaseMaterial->GetUsageByFlag(Usage);
#endif
    }

    // Prefixed because this module is built with Unity enabled.
    FSkeletalMeshCreateResult GeometrySkeletalAssetCreate_Fail(
        const TCHAR* ErrorCode, FString ErrorMessage)
    {
        FSkeletalMeshCreateResult Result;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = MoveTemp(ErrorMessage);
        return Result;
    }

    FSkeletalMeshCreateResult& GeometrySkeletalAssetCreate_FailInPlace(
        FSkeletalMeshCreateResult& Result, const TCHAR* ErrorCode, FString ErrorMessage)
    {
        Result.bSuccess = false;
        Result.Asset = nullptr;
        Result.ErrorCode = ErrorCode;
        Result.ErrorMessage = MoveTemp(ErrorMessage);
        return Result;
    }

    const UPwModelAssetUserData* GeometrySkeletalAssetCreate_ReadProvenance(UObject* ExistingObject)
    {
        USkeletalMesh* ExistingMesh = Cast<USkeletalMesh>(ExistingObject);
        if (!ExistingMesh)
        {
            return nullptr;
        }

        return Cast<UPwModelAssetUserData>(
            ExistingMesh->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()));
    }

    void GeometrySkeletalAssetCreate_WriteProvenance(
        USkeletalMesh* Mesh, const FString& SourcePath, const FString& SourceHash,
        TArrayView<const FPwSourceStateEntry> GeneratedState)
    {
        if (!Mesh || SourcePath.IsEmpty())
        {
            return;
        }

        IInterface_AssetUserData* UserDataOwner = Cast<IInterface_AssetUserData>(Mesh);
        if (!UserDataOwner)
        {
            return;
        }

        UPwModelAssetUserData* Stamp = Cast<UPwModelAssetUserData>(
            UserDataOwner->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()));
        if (!Stamp)
        {
            Stamp = NewObject<UPwModelAssetUserData>(Mesh);
            UserDataOwner->AddAssetUserData(Stamp);
        }

        Stamp->SourcePath = SourcePath;
        Stamp->SourceHash = SourceHash;
        Stamp->GeneratedStateVersion = PwSourceRecompileGuard::StateVersion;
        Stamp->GeneratedState = PwSourceRecompileGuard::MakeStateMap(GeneratedState);
    }

    FPwSourceStateEntry SkeletalMeshState(
        FString Key, FString Field, FString Value, FString Origin, bool bAssetPath = false)
    {
        FPwSourceStateEntry Entry;
        Entry.Key = MoveTemp(Key);
        Entry.Field = MoveTemp(Field);
        Entry.Value = MoveTemp(Value);
        Entry.Origin = MoveTemp(Origin);
        Entry.bAssetPath = bAssetPath;
        return Entry;
    }

    TArray<FPwSourceStateEntry> DesiredSkeletalMeshState(
        const FSkeletalMeshCreateSpec& Spec)
    {
        TArray<FPwSourceStateEntry> State;
        State.Add(SkeletalMeshState(TEXT("skeleton"), TEXT("skeleton"),
            Spec.Skeleton ? Spec.Skeleton->GetPathName() : FString(),
            TEXT("the bound skeleton can be changed by skeletal-mesh authoring tools")));
        for (int32 Index = 0; Index < Spec.MaterialSlots.Num(); ++Index)
        {
            const FString& Slot = Spec.MaterialSlots[Index];
            const FString* Binding = Spec.MaterialBindings.Find(Slot);
            State.Add(SkeletalMeshState(FString::Printf(TEXT("material:%d"), Index),
                FString::Printf(TEXT("material[%s]"), *Slot), Binding ? *Binding : FString(),
                TEXT("materials can be written by skeletal-mesh authoring verbs or the Skeletal Mesh editor"),
                /*bAssetPath=*/true));
        }
        return State;
    }

    TArray<FPwSourceStateEntry> CurrentSkeletalMeshState(const USkeletalMesh* Mesh)
    {
        TArray<FPwSourceStateEntry> State;
        if (!Mesh)
        {
            return State;
        }
        State.Add(SkeletalMeshState(TEXT("skeleton"), TEXT("skeleton"),
            Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : FString(),
            TEXT("the bound skeleton can be changed by skeletal-mesh authoring tools")));
        const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
        for (int32 Index = 0; Index < Materials.Num(); ++Index)
        {
            const FSkeletalMaterial& Material = Materials[Index];
            const FString Slot = Material.MaterialSlotName.ToString();
            State.Add(SkeletalMeshState(FString::Printf(TEXT("material:%d"), Index),
                FString::Printf(TEXT("material[%s]"), *Slot),
                Material.MaterialInterface ? Material.MaterialInterface->GetPathName() : FString(),
                TEXT("materials can be written by skeletal-mesh authoring verbs or the Skeletal Mesh editor"),
                /*bAssetPath=*/true));
        }
        if (const UPhysicsAsset* Physics = Mesh->GetPhysicsAsset())
        {
            State.Add(SkeletalMeshState(TEXT("physics_asset"), TEXT("physicsAsset"),
                Physics->GetPathName(),
                TEXT("physics assets can be assigned by physics authoring verbs or the Skeletal Mesh editor")));
        }
        for (const UMorphTarget* Morph : Mesh->GetMorphTargets())
        {
            if (Morph)
            {
                State.Add(SkeletalMeshState(TEXT("morph_target:") + Morph->GetName(),
                    FString::Printf(TEXT("morphTarget[%s]"), *Morph->GetName()), TEXT("present"),
                    TEXT("morph targets can be written by skeletal-mesh authoring tools")));
            }
        }
        for (const USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList())
        {
            if (Socket)
            {
                State.Add(SkeletalMeshState(TEXT("socket:") + Socket->SocketName.ToString(),
                    FString::Printf(TEXT("socket[%s]"), *Socket->SocketName.ToString()),
                    Socket->BoneName.ToString(),
                    TEXT("mesh sockets can be written by skeleton.create_socket or the Skeletal Mesh editor")));
            }
        }
        State.Add(SkeletalMeshState(TEXT("lod_count"), TEXT("lodCount"),
            FString::FromInt(Mesh->GetLODNum()),
            TEXT("LODs can be added by skeletal-mesh authoring tools or the Skeletal Mesh editor")));
        return State;
    }

    void GeometrySkeletalAssetCreate_CaptureClearedFeatures(
        USkeletalMesh* ExistingMesh, FSkeletalMeshCreateResult& Result,
        UPhysicsAsset*& OutPhysicsAsset, TArray<FString>& OutPriorMaterialPaths)
    {
        OutPhysicsAsset = nullptr;
        OutPriorMaterialPaths.Reset();
        if (!ExistingMesh)
        {
            return;
        }

        OutPhysicsAsset = ExistingMesh->GetPhysicsAsset();
        if (OutPhysicsAsset)
        {
            Result.ClearedFeatures.Add(OutPhysicsAsset->GetPathName());
        }

        for (UMorphTarget* MorphTarget : ExistingMesh->GetMorphTargets())
        {
            if (MorphTarget)
            {
                Result.ClearedFeatures.AddUnique(MorphTarget->GetName());
            }
        }

        // Material bindings are captured but NOT reported here, because the rebuild usually puts
        // them back: a source that names the same slots rebinds them, and reporting those as
        // cleared would make ClearedFeatures noise on every ordinary recompile. What is reported
        // is the difference measured afterwards - see the survivor check after creation.
        for (const FSkeletalMaterial& Material : ExistingMesh->GetMaterials())
        {
            if (Material.MaterialInterface)
            {
                OutPriorMaterialPaths.AddUnique(Material.MaterialInterface->GetPathName());
            }
        }
    }

    // Any material the occupant had bound that is not bound on the rebuilt asset. The engine
    // utility empties GetMaterials() on the reuse path, so a .pwmodel that names no material at
    // all silently drops every binding the mesh carried - the exact silent loss ClearedFeatures
    // exists to prevent, and the one case it did not cover.
    void GeometrySkeletalAssetCreate_ReportDroppedMaterials(
        const USkeletalMesh* NewMesh, const TArray<FString>& PriorMaterialPaths,
        FSkeletalMeshCreateResult& Result)
    {
        if (PriorMaterialPaths.IsEmpty())
        {
            return;
        }

        TSet<FString> StillBound;
        if (NewMesh)
        {
            for (const FSkeletalMaterial& Material : NewMesh->GetMaterials())
            {
                if (Material.MaterialInterface)
                {
                    StillBound.Add(Material.MaterialInterface->GetPathName());
                }
            }
        }

        for (const FString& Path : PriorMaterialPaths)
        {
            if (!StillBound.Contains(Path))
            {
                Result.ClearedFeatures.AddUnique(Path);
            }
        }
    }

    bool GeometrySkeletalAssetCreate_ReadImportedCounts(
        USkeletalMesh* Mesh, int32& OutTriangleCount, int32& OutVertexCount)
    {
        const FSkeletalMeshModel* ImportedModel = Mesh ? Mesh->GetImportedModel() : nullptr;
        if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(0))
        {
            return false;
        }

        const FSkeletalMeshLODModel& LOD0 = ImportedModel->LODModels[0];
        OutTriangleCount = 0;
        OutVertexCount = 0;
        for (const FSkelMeshSection& Section : LOD0.Sections)
        {
            OutTriangleCount += static_cast<int32>(Section.NumTriangles);
            OutVertexCount += Section.NumVertices;
        }

        return LOD0.Sections.Num() > 0 && OutTriangleCount > 0 && OutVertexCount > 0;
    }

    int32 GeometrySkeletalAssetCreate_RequiredMaterialSlotCount(
        const UE::Geometry::FDynamicMesh3& Mesh)
    {
        int32 RequiredSlotCount = 1;
        const UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs =
            Mesh.HasAttributes() ? Mesh.Attributes()->GetMaterialID() : nullptr;
        if (!MaterialIDs)
        {
            return RequiredSlotCount;
        }

        for (const int32 TriangleID : Mesh.TriangleIndicesItr())
        {
            RequiredSlotCount = FMath::Max(
                RequiredSlotCount, MaterialIDs->GetValue(TriangleID) + 1);
        }
        return RequiredSlotCount;
    }

    void GeometrySkeletalAssetCreate_AccumulateRenderMaterialRequirements(
        const USkeletalMesh* Mesh, int32& InOutRequiredMaterialSlotCount)
    {
        const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
        if (!RenderData)
        {
            return;
        }

        for (const FSkeletalMeshLODRenderData& LODData : RenderData->LODRenderData)
        {
            for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
            {
                InOutRequiredMaterialSlotCount = FMath::Max(
                    InOutRequiredMaterialSlotCount, static_cast<int32>(Section.MaterialIndex) + 1);
            }
        }
    }

    bool GeometrySkeletalAssetCreate_ValidateRenderMaterialIndices(
        const USkeletalMesh* Mesh, int32 MaterialSlotCount)
    {
        const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
        if (!RenderData)
        {
            return true;
        }

        for (const FSkeletalMeshLODRenderData& LODData : RenderData->LODRenderData)
        {
            for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
            {
                if (Section.MaterialIndex >= MaterialSlotCount)
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool GeometrySkeletalAssetCreate_ReadMaterialCoverage(
        const USkeletalMesh* Mesh, FSkeletalMeshMaterialCoverage& OutCoverage)
    {
        OutCoverage = FSkeletalMeshMaterialCoverage();
        const FSkeletalMeshModel* ImportedModel = Mesh ? Mesh->GetImportedModel() : nullptr;
        if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(0))
        {
            return false;
        }

        OutCoverage.RequiredMaterialSlotCount = 1;
        for (const FSkeletalMeshLODModel& LODModel : ImportedModel->LODModels)
        {
            for (const FSkelMeshSection& Section : LODModel.Sections)
            {
                OutCoverage.RequiredMaterialSlotCount = FMath::Max(
                    OutCoverage.RequiredMaterialSlotCount,
                    static_cast<int32>(Section.MaterialIndex) + 1);
            }
        }
        GeometrySkeletalAssetCreate_AccumulateRenderMaterialRequirements(
            Mesh, OutCoverage.RequiredMaterialSlotCount);

        const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
        OutCoverage.MaterialSlotCount = Materials.Num();
        return true;
    }

    // The engine utility is allowed to merge missing bones and to fill an empty preview slot on
    // the referenced skeleton.  Package dirty state cannot tell us whether this invocation made
    // that change: the package may already have unrelated author edits.  Serialize the object
    // before and after the utility instead, so an unchanged shared skeleton is not rewritten.
    bool GeometrySkeletalAssetCreate_CaptureSkeletonState(
        const USkeleton* Skeleton, TArray<uint8>& OutState)
    {
        OutState.Reset();
        if (!Skeleton)
        {
            return false;
        }

        FObjectWriter Writer(OutState);
        // Set this before Serialize: transient skeleton caches are rebuilt by the engine merge
        // routine and must not turn every compile into a durable package change.
        Writer.SetIsPersistent(true);
        Writer.ArNoDelta = true;
        const_cast<USkeleton*>(Skeleton)->Serialize(Writer);
        return true;
    }
}

bool EnsureSkeletalMeshMaterialSlots(
    USkeletalMesh* Mesh, FSkeletalMeshMaterialCoverage& OutCoverage)
{
    if (!GeometrySkeletalAssetCreate_ReadMaterialCoverage(Mesh, OutCoverage))
    {
        return false;
    }

    TArray<FSkeletalMaterial> Materials = Mesh->GetMaterials();

    UMaterialInterface* FallbackMaterial = nullptr;
    for (const FSkeletalMaterial& Material : Materials)
    {
        if (Material.MaterialInterface)
        {
            FallbackMaterial = Material.MaterialInterface;
            break;
        }
    }
    if (!FallbackMaterial)
    {
        FallbackMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
    }
    if (!FallbackMaterial)
    {
        return false;
    }

    bool bChanged = false;
    for (FSkeletalMaterial& Material : Materials)
    {
        if (!Material.MaterialInterface)
        {
            Material.MaterialInterface = FallbackMaterial;
            bChanged = true;
        }
    }

    while (Materials.Num() < OutCoverage.RequiredMaterialSlotCount)
    {
        const int32 SlotIndex = Materials.Num();
        const FName SlotName(*FString::Printf(TEXT("Material%d"), SlotIndex));
        Materials.Emplace(FallbackMaterial, SlotName, SlotName);
        bChanged = true;
    }

    if (bChanged)
    {
        Mesh->SetMaterials(Materials);
    }

    OutCoverage.MaterialSlotCount = Materials.Num();
    return GeometrySkeletalAssetCreate_ValidateRenderMaterialIndices(Mesh, Materials.Num());
}

FSkeletalMeshCreateResult CreateSkeletalMesh(UDynamicMesh* Mesh, const FSkeletalMeshCreateSpec& Spec)
{
    if (!Mesh)
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("No source mesh supplied"));
    }
    if (Spec.AssetPath.IsEmpty())
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath required"));
    }

    FText BadPackageNameReason;
    if (!FPackageName::IsValidTextForLongPackageName(Spec.AssetPath, &BadPackageNameReason))
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("Invalid asset path '%s': %s"),
                *Spec.AssetPath, *BadPackageNameReason.ToString()));
    }
    if (Spec.AssetPath.StartsWith(TEXT("/Engine/")) &&
        !Spec.AssetPath.StartsWith(TEXT("/Engine/Transient")))
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_SECURITY_VIOLATION,
            TEXT("Refusing to write a generated SkeletalMesh into /Engine/ - target a /Game/ path"));
    }
    if (!Spec.Skeleton)
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("A skeleton is required"));
    }
    if (Spec.Skeleton->GetReferenceSkeleton().GetNum() <= 0)
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_SKELETON_HAS_NO_BONES,
            FString::Printf(TEXT("Skeleton '%s' has no bones"), *Spec.Skeleton->GetPathName()));
    }
    if (Mesh->GetTriangleCount() == 0)
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_ASSET_CREATION_FAILED,
            TEXT("Failed to create SkeletalMesh asset - the source mesh has no triangles"));
    }

    const FString PackageName = Spec.AssetPath;
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
    if (AssetName.IsEmpty())
    {
        return GeometrySkeletalAssetCreate_Fail(
            ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("Asset path '%s' has no asset name"), *Spec.AssetPath));
    }

    FSkeletalMeshCreateResult Out;
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        PackageName, AssetName, USkeletalMesh::StaticClass(), /*bOverwriteRequested=*/false);
    if (Resolution.IsRejected())
    {
        return GeometrySkeletalAssetCreate_FailInPlace(
            Out, *Resolution.ErrorCode, Resolution.ErrorMessage);
    }

    const bool bUpdateInPlace =
        Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace;
    USkeletalMesh* ExistingMesh = bUpdateInPlace ? Cast<USkeletalMesh>(Resolution.Existing) : nullptr;
    if (bUpdateInPlace && !ExistingMesh)
    {
        return GeometrySkeletalAssetCreate_FailInPlace(
            Out, ErrorCodes::ERR_ASSET_CREATION_FAILED,
            FString::Printf(TEXT("The existing asset at %s could not be loaded as a SkeletalMesh"),
                *PackageName));
    }

    const UPwModelAssetUserData* ExistingStamp = bUpdateInPlace
        ? GeometrySkeletalAssetCreate_ReadProvenance(Resolution.Existing) : nullptr;
    const bool bSameSource = ExistingStamp && !Spec.SourcePath.IsEmpty() &&
        ExistingStamp->SourcePath == Spec.SourcePath;

    bool bGuardAllowsWrite = true;
    if (bUpdateInPlace)
    {
        FPwSourceRecompileGuardRequest Guard;
        Guard.FormatName = TEXT(".pwmodel");
        Guard.AssetPath = PackageName;
        Guard.bSameSourceRecompile = bSameSource;
        Guard.bTakeover = !bSameSource;
        Guard.bOverwrite = Spec.bOverwrite;
        Guard.CurrentSourcePath = ExistingStamp ? ExistingStamp->SourcePath : FString();
        Guard.RequestedSourcePath = Spec.SourcePath;
        Guard.BaselineVersion = ExistingStamp ? ExistingStamp->GeneratedStateVersion : 0;
        Guard.Baseline = ExistingStamp ? &ExistingStamp->GeneratedState : nullptr;
        Guard.Current = CurrentSkeletalMeshState(ExistingMesh);
        Guard.Desired = DesiredSkeletalMeshState(Spec);
        bGuardAllowsWrite = PwSourceRecompileGuard::Check(Guard, Out.Diagnostics);
    }

    if (bUpdateInPlace)
    {
        if (!bSameSource && !Spec.bOverwrite)
        {
            FString Why;
            if (!ExistingStamp)
            {
                Why = FString::Printf(
                    TEXT("A SkeletalMesh already exists at %s and carries no PinWright Model provenance stamp"),
                    *PackageName);
            }
            else
            {
                Why = FString::Printf(
                    TEXT("The asset at %s was generated from '%s', not '%s'"),
                    *PackageName, *ExistingStamp->SourcePath, *Spec.SourcePath);
            }
            if (!bGuardAllowsWrite && Out.Diagnostics.Num() > 0)
            {
                Why += TEXT(". ") + Out.Diagnostics.Last().Message;
            }
            else
            {
                Why += TEXT(" - pass overwrite=true to rebuild it in place (its referencers are preserved)");
            }
            return GeometrySkeletalAssetCreate_FailInPlace(
                Out, ErrorCodes::ERR_ASSET_EXISTS, MoveTemp(Why));
        }
    }

    if (!bGuardAllowsWrite)
    {
        return GeometrySkeletalAssetCreate_FailInPlace(
            Out, ErrorCodes::ERR_ASSET_DATA_INVALID, Out.Diagnostics.Last().Message);
    }

    const bool bHasUVs = GeometryUtils::EnsureMeshHasUVs(Mesh);
    if (!bHasUVs)
    {
        Out.Warnings.Add(
            TEXT("Mesh carries no usable UV channel 0; normal/tangent recompute disabled for the bake"));
    }

    TArray<FSkeletalMaterial> SkeletalMaterials;
    UE::Geometry::FDynamicMesh3 MeshCopy;
    Mesh->ProcessMesh([&MeshCopy](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        MeshCopy = ReadMesh;
    });

    // The skeletal builder validates section material indices against the material array while
    // it builds.  Supplying only the explicit slots (or the engine's one implicit slot) lets a
    // sparse dynamic-mesh ID such as 2 be clamped before the post-build repair can see it.
    const int32 RequiredMaterialSlotCount =
        GeometrySkeletalAssetCreate_RequiredMaterialSlotCount(MeshCopy);
    const int32 NumMaterialSlots = FMath::Max(
        FMath::Max(1, Spec.MaterialSlots.Num()), RequiredMaterialSlotCount);
    SkeletalMaterials.Reserve(NumMaterialSlots);
    TArray<UMaterialInterface*> MaterialsNeedingSkeletalUsageSave;
    for (int32 SlotIndex = 0; SlotIndex < NumMaterialSlots; ++SlotIndex)
    {
        const bool bExplicitSlot = Spec.MaterialSlots.IsValidIndex(SlotIndex);
        const FString SlotName = bExplicitSlot
            ? Spec.MaterialSlots[SlotIndex]
            : FString::Printf(TEXT("Material%d"), SlotIndex);
        UMaterialInterface* BoundMaterial = nullptr;
        const FString* BoundPath = bExplicitSlot ? Spec.MaterialBindings.Find(SlotName) : nullptr;
        if (!BoundPath || BoundPath->IsEmpty())
        {
            if (bExplicitSlot)
            {
                Out.UnboundSlots.Add(SlotName);
            }
        }
        else
        {
            BoundMaterial = LoadObject<UMaterialInterface>(nullptr, **BoundPath);
            if (!BoundMaterial)
            {
                Out.UnboundSlots.Add(SlotName);
                Out.Warnings.Add(FString::Printf(
                    TEXT("Material slot '%s' is bound to '%s', which could not be loaded; using the default material"),
                    *SlotName, **BoundPath));
            }
            else if (!GeometrySkeletalAssetCreate_HasUsage(BoundMaterial, MATUSAGE_SkeletalMesh))
            {
                // SkeletalMesh's render path checks this usage on the material interface and
                // may auto-enable it in the editor.  Do that before the build so a material
                // that cannot support the skeletal permutation fails at the authoring boundary,
                // rather than producing an asset that silently falls back to the default
                // material in a cooked game.
                if (!BoundMaterial->CheckMaterialUsage(MATUSAGE_SkeletalMesh) ||
                    !GeometrySkeletalAssetCreate_HasUsage(BoundMaterial, MATUSAGE_SkeletalMesh))
                {
                    return GeometrySkeletalAssetCreate_FailInPlace(
                        Out, ErrorCodes::ERR_ASSET_CREATION_FAILED,
                        FString::Printf(
                            TEXT("Material '%s' does not support SkeletalMesh usage; the skeletal asset would use the default material"),
                            *BoundMaterial->GetPathName()));
                }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
                MaterialsNeedingSkeletalUsageSave.AddUnique(BoundMaterial);
#else
                // Before 5.8 a material instance carries no usage flags of its own:
                // UMaterialInstance::CheckMaterialUsage forwards straight to the base UMaterial
                // (MaterialInstance.cpp:1704). The object that changed - and so the one that has
                // to reach disk for the usage to survive a reload - is the base material.
                if (UMaterial* BaseMaterial = BoundMaterial->GetMaterial())
                {
                    MaterialsNeedingSkeletalUsageSave.AddUnique(BaseMaterial);
                }
#endif
            }
        }

        const FName MaterialSlotName = SlotName.IsEmpty() ? NAME_None : FName(*SlotName);
        SkeletalMaterials.Emplace(BoundMaterial, MaterialSlotName, MaterialSlotName);
    }

    UPhysicsAsset* ExistingPhysicsAsset = nullptr;
    TArray<FString> PriorMaterialPaths;
    GeometrySkeletalAssetCreate_CaptureClearedFeatures(
        ExistingMesh, Out, ExistingPhysicsAsset, PriorMaterialPaths);

    UE::AssetUtils::FSkeletalMeshAssetOptions Options;
    Options.UsePackage = (bUpdateInPlace && Resolution.Existing)
        ? Resolution.Existing->GetPackage()
        : nullptr;
    Options.NewAssetPath = PackageName;
    Options.Skeleton = Spec.Skeleton;
    Options.NumSourceModels = 1;
    Options.NumMaterialSlots = NumMaterialSlots;
    Options.bEnableRecomputeNormals = Spec.bRecomputeNormals && bHasUVs;
    Options.bEnableRecomputeTangents = Spec.bRecomputeTangents && bHasUVs;
    Options.SkeletalMaterials = MoveTemp(SkeletalMaterials);
    Options.SourceMeshes.DynamicMeshes.Add(&MeshCopy);

    TArray<uint8> SkeletonStateBefore;
    const bool bCapturedSkeletonStateBefore =
        GeometrySkeletalAssetCreate_CaptureSkeletonState(Spec.Skeleton, SkeletonStateBefore);

    // Unlike the static utility, UE 5.8 exposes no bDeferPostEditChange field for skeletal
    // creation. CreateSkeletalMeshAsset owns the one FScopedSkeletalMeshPostEditChange scope
    // around conversion; calling PostEditChange here would build the asset a second time.
    UE::AssetUtils::FSkeletalMeshResults CreateResults;
    const UE::AssetUtils::ECreateSkeletalMeshResult CreateOutcome =
        UE::AssetUtils::CreateSkeletalMeshAsset(Options, CreateResults);
    if (CreateOutcome != UE::AssetUtils::ECreateSkeletalMeshResult::Ok ||
        !CreateResults.SkeletalMesh)
    {
        return GeometrySkeletalAssetCreate_FailInPlace(
            Out, ErrorCodes::ERR_ASSET_CREATION_FAILED,
            FString::Printf(TEXT("Failed to create SkeletalMesh asset at %s"), *PackageName));
    }

    USkeletalMesh* NewMesh = CreateResults.SkeletalMesh;

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UE 5.4 made FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions pin
    // the LOD it creates to identity reduction (NumOfTrianglesPercentage/NumOfVertPercentage = 1,
    // MaxDeviationPercentage = 0, LODHysteresis = 0.02). On 5.3 that same function leaves
    // FSkeletalMeshOptimizationSettings at its struct defaults, which make LOD 0 reduction ACTIVE:
    // the build then runs the simplifier over the mesh just converted ("LogSkeletalMeshReduction:
    // Reducing skeletal mesh for LOD 0") and welds sections that differ only by material, so a
    // two-material source is persisted as one section. Restore the 5.4 values and rebuild, so this
    // verb writes the caller's geometry rather than a simplification of it on every engine.
    if (FSkeletalMeshLODInfo* ConvertedLODInfo = NewMesh->GetLODInfo(0))
    {
        ConvertedLODInfo->ReductionSettings.NumOfTrianglesPercentage = 1.0f;
        ConvertedLODInfo->ReductionSettings.NumOfVertPercentage = 1.0f;
        ConvertedLODInfo->ReductionSettings.MaxDeviationPercentage = 0.0f;
        ConvertedLODInfo->LODHysteresis = 0.02f;
        NewMesh->Build();
    }
#endif

    if (!EnsureSkeletalMeshMaterialSlots(NewMesh, Out.MaterialCoverage))
    {
        return GeometrySkeletalAssetCreate_FailInPlace(
            Out, ErrorCodes::ERR_ASSET_DATA_INVALID,
            FString::Printf(TEXT("Created SkeletalMesh at %s has no readable material coverage"),
                *PackageName));
    }
    TArray<uint8> SkeletonStateAfter;
    const bool bCapturedSkeletonStateAfter =
        GeometrySkeletalAssetCreate_CaptureSkeletonState(Spec.Skeleton, SkeletonStateAfter);
    const bool bSkeletonChanged = !bCapturedSkeletonStateBefore || !bCapturedSkeletonStateAfter
        || SkeletonStateBefore != SkeletonStateAfter;

    // Measured after the rebuild, not predicted before it: only the bindings that failed to
    // come back are reported, so an ordinary recompile that rebinds every slot stays quiet.
    GeometrySkeletalAssetCreate_ReportDroppedMaterials(NewMesh, PriorMaterialPaths, Out);
    GeometrySkeletalAssetCreate_WriteProvenance(
        NewMesh, Spec.SourcePath, Spec.SourceHash, CurrentSkeletalMeshState(NewMesh));

    if (!GeometrySkeletalAssetCreate_ReadImportedCounts(
            NewMesh, Out.TriangleCount, Out.VertexCount))
    {
        return GeometrySkeletalAssetCreate_FailInPlace(
            Out, ErrorCodes::ERR_ASSET_DATA_INVALID,
            FString::Printf(TEXT("Created SkeletalMesh at %s has no readable imported LOD 0 sections"),
                *PackageName));
    }

    // CreateSkeletalMeshAsset does not publish the asset. Re-announcing an update refreshes the
    // registry entry without deleting the object that existing referencers hold.
    FAssetRegistryModule::AssetCreated(NewMesh);

    if (Spec.bSave)
    {
        // CheckMaterialUsage above changes the referenced material package, not the skeletal
        // mesh package.  Saving only the output mesh therefore leaves the newly-created shader
        // permutation in memory and lets the next editor launch/cook fall back to the default
        // material again.  Persist exactly the materials whose usage flag this invocation added;
        // materials that were already valid are not rewritten.
        for (UMaterialInterface* Material : MaterialsNeedingSkeletalUsageSave)
        {
            FString MaterialPackageName;
            EAssetSaveState MaterialSaveState = EAssetSaveState::NotRequested;
            if (!SaveAssetToDiskReportingPresence(
                    Material, /*bForce=*/true, &MaterialPackageName, nullptr, &MaterialSaveState))
            {
                return GeometrySkeletalAssetCreate_FailInPlace(
                    Out, ErrorCodes::ERR_ASSET_CREATION_FAILED,
                    FString::Printf(
                        TEXT("SkeletalMesh material usage was enabled for '%s' but its package could not be saved (state %s)"),
                        *Material->GetPathName(), AssetSaveStateToWire(MaterialSaveState)));
            }
        }

        Out.bSavedToDisk = SaveAssetToDiskReportingPresence(
            NewMesh, /*bForce=*/true, &Out.PackageName, &Out.SizeBytes, &Out.SaveState);
    }
    else
    {
        NewMesh->MarkPackageDirty();
        Out.PackageName = NewMesh->GetOutermost()
            ? NewMesh->GetOutermost()->GetName()
            : FString();
        Out.SaveState = EAssetSaveState::NotRequested;
    }
    Out.bPendingFlush = Spec.bSave && !Out.bSavedToDisk;

    Out.SkeletonPackageName = Spec.Skeleton->GetOutermost()
        ? Spec.Skeleton->GetOutermost()->GetName()
        : FString();
    if (Spec.bSave && bSkeletonChanged)
    {
        SaveAssetToDiskReportingPresence(
            Spec.Skeleton, /*bForce=*/true, &Out.SkeletonPackageName,
            nullptr, &Out.SkeletonSaveState);
    }
    else if (!Spec.bSave && bSkeletonChanged)
    {
        Spec.Skeleton->MarkPackageDirty();
        Out.SkeletonSaveState = EAssetSaveState::NotRequested;
    }
    else
    {
        // No skeleton property changed during this invocation.  In particular, do not force a
        // save of a shared or engine-owned package merely because this mesh references it.
        Out.SkeletonSaveState = EAssetSaveState::NotRequested;
    }

    Out.Asset = NewMesh;
    Out.bUpdatedInPlace = bUpdateInPlace;
    Out.bSuccess = true;
    return Out;
}
