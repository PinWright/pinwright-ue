// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/ThumbnailPreviewOverride.h"

#include "Handlers/ErrorCodes.h"

#include "Compat/EngineVersionCompat.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace PinWrightThumbnail
{
    namespace
    {
        // Reflected field names on USceneThumbnailInfo / USceneThumbnailInfoWithPrimitive.
        const TCHAR* const ThumbnailInfoFieldName = TEXT("ThumbnailInfo");
        const TCHAR* const PrimitiveTypeFieldName = TEXT("PrimitiveType");
        const TCHAR* const PreviewMeshFieldName = TEXT("PreviewMesh");
        const TCHAR* const UserModifiedShapeFieldName = TEXT("bUserModifiedShape");
        const TCHAR* const OrbitPitchFieldName = TEXT("OrbitPitch");
        const TCHAR* const OrbitYawFieldName = TEXT("OrbitYaw");
        const TCHAR* const OrbitZoomFieldName = TEXT("OrbitZoom");

        FNumericProperty* FindNumeric(const UObject* Object, const TCHAR* FieldName)
        {
            return Object
                ? CastField<FNumericProperty>(Object->GetClass()->FindPropertyByName(FieldName))
                : nullptr;
        }

        bool CopyFloatField(const UObject* From, UObject* To, const TCHAR* FieldName)
        {
            FNumericProperty* SourceProp = FindNumeric(From, FieldName);
            FNumericProperty* DestProp = FindNumeric(To, FieldName);
            if (!SourceProp || !DestProp)
            {
                return false;
            }
            DestProp->SetFloatingPointPropertyValue(
                DestProp->ContainerPtrToValuePtr<void>(To),
                SourceProp->GetFloatingPointPropertyValue(
                    SourceProp->ContainerPtrToValuePtr<void>(From)));
            return true;
        }

        // Whether the shape this ThumbnailInfo now names is the engine's thumbnail PLANE.
        // TPT_Plane, plus the TPT_None branch whose PreviewMesh does not resolve to a
        // UStaticMesh: FMaterialThumbnailScene falls back to the same EditorPlane mesh under the
        // same constant rotation there (ThumbnailHelpers.cpp:357-362 and :380-383).
        bool ResolvedShapeIsPlane(const UObject* Info)
        {
            FNumericProperty* PrimitiveProp = FindNumeric(Info, PrimitiveTypeFieldName);
            if (!PrimitiveProp)
            {
                // No PrimitiveType field at all: this asset's thumbnail scene renders the asset
                // itself, never a preview primitive.
                return false;
            }

            const uint8 Primitive = static_cast<uint8>(PrimitiveProp->GetSignedIntPropertyValue(
                PrimitiveProp->ContainerPtrToValuePtr<void>(Info)));
            if (Primitive == static_cast<uint8>(TPT_Plane))
            {
                return true;
            }
            if (Primitive != static_cast<uint8>(TPT_None))
            {
                return false;
            }

            FStructProperty* MeshProp = CastField<FStructProperty>(
                Info->GetClass()->FindPropertyByName(PreviewMeshFieldName));
            if (!MeshProp || !MeshProp->Struct ||
                MeshProp->Struct->GetFName() != FName(TEXT("SoftObjectPath")))
            {
                return true;
            }
            const FSoftObjectPath& Mesh =
                *MeshProp->ContainerPtrToValuePtr<FSoftObjectPath>(Info);
            return Cast<UStaticMesh>(Mesh.ResolveObject()) == nullptr;
        }
    }

    bool WillForcePlanePreview(UObject* Asset, FString& OutReason)
    {
        UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Asset);
        if (!MaterialInterface)
        {
            return false;
        }

        // Exactly FMaterialThumbnailScene::SetMaterialInterface's own gate
        // (ThumbnailHelpers.cpp:329-338): the BASE material's answer, not the named asset's, so
        // an instance is judged by its master's rules.
        UMaterial* BaseMaterial = MaterialInterface->GetBaseMaterial();
        const bool bForced = BaseMaterial ? BaseMaterial->ShouldForcePlanePreview()
                                          : MaterialInterface->ShouldForcePlanePreview();
        if (!bForced)
        {
            return false;
        }

        const UObject* Culprit = BaseMaterial ? static_cast<UObject*>(BaseMaterial)
                                              : static_cast<UObject*>(MaterialInterface);
        if (BaseMaterial && BaseMaterial->IsUIMaterial())
        {
            OutReason = FString::Printf(
                TEXT("the base material '%s' is a UI-domain material, which the engine always "
                     "draws on a flat camera-facing plane"),
                *Culprit->GetName());
        }
        else
        {
            OutReason = FString::Printf(
                TEXT("the base material '%s' forces a plane preview (particle-sprite or Niagara "
                     "usage, or an explicit SetShouldForcePlanePreview)"),
                *Culprit->GetName());
        }
        return true;
    }

    bool ParsePrimitiveName(const FString& Name, uint8& OutPrimitiveType, bool& OutIsCustomMesh)
    {
        OutIsCustomMesh = false;

        // The six EThumbnailPrimType values plus "mesh". "mesh" maps onto TPT_None, which is
        // the branch FMaterialThumbnailScene::SetMaterialInterface uses to read PreviewMesh;
        // "none" is deliberately NOT exposed because on its own it renders a bare plane and
        // would read as "no primitive" to a caller.
        if (Name.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
        {
            OutPrimitiveType = static_cast<uint8>(TPT_Sphere);
            return true;
        }
        if (Name.Equals(TEXT("cube"), ESearchCase::IgnoreCase))
        {
            OutPrimitiveType = static_cast<uint8>(TPT_Cube);
            return true;
        }
        if (Name.Equals(TEXT("plane"), ESearchCase::IgnoreCase))
        {
            OutPrimitiveType = static_cast<uint8>(TPT_Plane);
            return true;
        }
        if (Name.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase))
        {
            OutPrimitiveType = static_cast<uint8>(TPT_Cylinder);
            return true;
        }
        // TPT_ShaderBall joined EThumbnailPrimType in UE 5.8. On older engines the shape does
        // not exist, so the spelling is refused like any other unknown name rather than being
        // silently redirected to a different primitive.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        if (Name.Equals(TEXT("shaderBall"), ESearchCase::IgnoreCase))
        {
            OutPrimitiveType = static_cast<uint8>(TPT_ShaderBall);
            return true;
        }
#endif
        if (Name.Equals(TEXT("mesh"), ESearchCase::IgnoreCase))
        {
            OutPrimitiveType = static_cast<uint8>(TPT_None);
            OutIsCustomMesh = true;
            return true;
        }
        return false;
    }

    FString PrimitiveNameList()
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        return TEXT("sphere, cube, plane, cylinder, shaderBall, mesh");
#else
        return TEXT("sphere, cube, plane, cylinder, mesh");
#endif
    }

    FScopedPreviewOverride::FScopedPreviewOverride(UObject* Asset,
        const FPreviewOverrideRequest& Request, FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (!Asset)
        {
            OutErrorCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
            OutErrorMessage = TEXT("No asset to override thumbnail settings on");
            return;
        }

        // No overrides requested: the guard is inert and the render sees exactly the asset's
        // own stored settings, byte-for-byte as before these parameters existed.
        if (Request.IsEmpty())
        {
            bValid = true;
            return;
        }

        AssetPtr = Asset;

        ThumbnailInfoProp =
            CastField<FObjectProperty>(Asset->GetClass()->FindPropertyByName(ThumbnailInfoFieldName));
        if (!ThumbnailInfoProp)
        {
            OutErrorCode = ErrorCodes::ERR_UNSUPPORTED_ASSET;
            OutErrorMessage = FString::Printf(
                TEXT("Asset class '%s' has no ThumbnailInfo, so primitive / azimuth / elevation / "
                     "zoom cannot be applied to it. Omit those parameters to render its default "
                     "thumbnail."),
                *Asset->GetClass()->GetName());
            return;
        }

        UObject* Existing = ThumbnailInfoProp->GetObjectPropertyValue(
            ThumbnailInfoProp->ContainerPtrToValuePtr<void>(Asset));

        // A primitive can only be applied through a ThumbnailInfo that actually carries the
        // field — USceneThumbnailInfoWithPrimitive, which materials use. When the asset has
        // none (or a plain USceneThumbnailInfo), synthesise a transient one for the scope,
        // but only for material-ish assets: handing a StaticMesh a primitive would set a
        // field its thumbnail scene never reads and silently do nothing.
        const bool bExistingCarriesPrimitive =
            Existing && Existing->GetClass()->FindPropertyByName(PrimitiveTypeFieldName) != nullptr;

        if (Request.bHasPrimitive && !bExistingCarriesPrimitive)
        {
            UClass* MaterialInterfaceClass =
                FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialInterface"));
            if (!MaterialInterfaceClass || !Asset->IsA(MaterialInterfaceClass))
            {
                OutErrorCode = ErrorCodes::ERR_UNSUPPORTED_ASSET;
                OutErrorMessage = FString::Printf(
                    TEXT("'primitive' is only supported for material assets; '%s' is a %s, whose "
                         "thumbnail always renders the asset itself. Use azimuth / elevation / zoom "
                         "to change its camera instead."),
                    *Asset->GetName(), *Asset->GetClass()->GetName());
                return;
            }

            UClass* InfoClass =
                FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.SceneThumbnailInfoWithPrimitive"));
            if (!InfoClass)
            {
                OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                OutErrorMessage = TEXT("USceneThumbnailInfoWithPrimitive class not found by "
                                       "reflection (engine layout drift?)");
                return;
            }

            UObject* NewInfo = NewObject<UObject>(Asset, InfoClass, NAME_None, RF_Transient);
            if (!NewInfo)
            {
                OutErrorCode = ErrorCodes::ERR_CREATE_FAILED;
                OutErrorMessage = TEXT("Failed to create a transient thumbnail info for the "
                                       "requested preview primitive");
                return;
            }

            // Carry the asset's existing camera over so an un-overridden angle keeps looking
            // the way the asset itself is set up.
            if (Existing)
            {
                CopyFloatField(Existing, NewInfo, OrbitPitchFieldName);
                CopyFloatField(Existing, NewInfo, OrbitYawFieldName);
                CopyFloatField(Existing, NewInfo, OrbitZoomFieldName);
            }

            PreviousThumbnailInfo.Reset(Existing);
            ThumbnailInfoProp->SetObjectPropertyValue(
                ThumbnailInfoProp->ContainerPtrToValuePtr<void>(Asset), NewInfo);
            bSwappedThumbnailInfo = true;
            Existing = NewInfo;
        }
        else if (!Existing)
        {
            // Camera-only override on an asset that has never had its thumbnail edited: the
            // engine would fall back to the class CDO, which we must not write to (it is
            // shared by every asset of that type). Synthesise a transient instance seeded
            // with the CDO defaults instead.
            UClass* InfoClass =
                FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.SceneThumbnailInfo"));
            if (!InfoClass)
            {
                OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                OutErrorMessage =
                    TEXT("USceneThumbnailInfo class not found by reflection (engine layout drift?)");
                return;
            }

            UObject* NewInfo = NewObject<UObject>(Asset, InfoClass, NAME_None, RF_Transient);
            if (!NewInfo)
            {
                OutErrorCode = ErrorCodes::ERR_CREATE_FAILED;
                OutErrorMessage =
                    TEXT("Failed to create a transient thumbnail info for the requested camera");
                return;
            }

            PreviousThumbnailInfo.Reset(nullptr);
            ThumbnailInfoProp->SetObjectPropertyValue(
                ThumbnailInfoProp->ContainerPtrToValuePtr<void>(Asset), NewInfo);
            bSwappedThumbnailInfo = true;
            Existing = NewInfo;
        }

        ActiveThumbnailInfo.Reset(Existing);

        // ---- primitive ----------------------------------------------------------------
        if (Request.bHasPrimitive)
        {
            PrimitiveTypeProp = FindNumeric(Existing, PrimitiveTypeFieldName);
            if (!PrimitiveTypeProp)
            {
                OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                OutErrorMessage = TEXT("SceneThumbnailInfoWithPrimitive.PrimitiveType not found by "
                                       "reflection (engine layout drift?)");
                Restore();
                return;
            }

            void* PrimitiveValuePtr = PrimitiveTypeProp->ContainerPtrToValuePtr<void>(Existing);
            SavedPrimitiveType =
                static_cast<uint8>(PrimitiveTypeProp->GetSignedIntPropertyValue(PrimitiveValuePtr));
            bSavedPrimitiveType = true;
            PrimitiveTypeProp->SetIntPropertyValue(
                PrimitiveValuePtr, static_cast<int64>(Request.PrimitiveType));

            if (!Request.PreviewMesh.IsNull())
            {
                PreviewMeshProp =
                    CastField<FStructProperty>(Existing->GetClass()->FindPropertyByName(PreviewMeshFieldName));
                // Identified by struct NAME, not TBaseStructure<FSoftObjectPath>::Get(): UE 5.8
                // declares no such specialization and FSoftObjectPath is a noexport USTRUCT with
                // no StaticStruct(), so the trait form does not compile.
                if (!PreviewMeshProp || !PreviewMeshProp->Struct ||
                    PreviewMeshProp->Struct->GetFName() != FName(TEXT("SoftObjectPath")))
                {
                    OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                    OutErrorMessage =
                        TEXT("SceneThumbnailInfoWithPrimitive.PreviewMesh not found by reflection, "
                             "or is no longer an FSoftObjectPath (engine layout drift?)");
                    Restore();
                    return;
                }

                FSoftObjectPath* PreviewMeshValue =
                    PreviewMeshProp->ContainerPtrToValuePtr<FSoftObjectPath>(Existing);
                SavedPreviewMesh = *PreviewMeshValue;
                bSavedPreviewMesh = true;
                *PreviewMeshValue = Request.PreviewMesh;
            }

            // A particle-sprite / Niagara material is force-planed by
            // UMaterial::ShouldForcePlanePreview unless the user has deliberately picked a
            // shape; an explicit `primitive` IS that deliberate pick. (A UI-domain material
            // is force-planed unconditionally and this flag does not rescue it.)
            UserModifiedShapeProp = CastField<FBoolProperty>(
                Existing->GetClass()->FindPropertyByName(UserModifiedShapeFieldName));
            if (UserModifiedShapeProp)
            {
                SavedUserModifiedShape = UserModifiedShapeProp->GetPropertyValue_InContainer(Existing);
                bSavedUserModifiedShape = true;
                UserModifiedShapeProp->SetPropertyValue_InContainer(Existing, true);
            }

            // ...and on the BASE material, which is the object the engine's gate actually
            // reads. FMaterialThumbnailScene::SetMaterialInterface computes bForcePlaneThumbnail
            // from GetBaseMaterial()->ShouldForcePlanePreview() (ThumbnailHelpers.cpp:333), and
            // UMaterial::ShouldForcePlanePreview reads bUserModifiedShape off ITS OWN
            // ThumbnailInfo (Material.cpp:7270-7281). For a material INSTANCE those are two
            // different objects: writing the flag only on the instance left the master's false,
            // the Niagara/particle rule stayed armed, and the requested primitive was thrown
            // away by the `bForcePlaneThumbnail ? TPT_Plane : ...` ternary at :340 — silently,
            // because the instance's PrimitiveType had been written and was read.
            ApplyBaseMaterialShapeFlag(Asset);
        }

        // ---- resolved shape ------------------------------------------------------------
        // What the render will really draw, resolved here rather than assumed from the request.
        // The base-material flag above defeats the defeatable force-plane rules, so anything
        // still forcing a plane (UI domain, an explicit SetShouldForcePlanePreview) is a
        // substitution the caller has to be told about.
        bForcedToPlane = WillForcePlanePreview(Asset, ForcePlaneReason);
        const bool bResolvedShapeIsPlane = bForcedToPlane || ResolvedShapeIsPlane(Existing);

        // ---- camera -------------------------------------------------------------------
        // The engine's thumbnail plane is a zero-thickness quad pinned to ONE fixed attitude:
        // SetMaterialInterface rotates it by the constant FRotator(0, -90, 0)
        // (ThumbnailHelpers.cpp:380-383) and it does not track the orbit. Orbiting away from
        // that attitude renders it edge on — at elevation 89 the whole frame is a ~2px sliver,
        // which reads as a broken material rather than a misplaced camera. The engine guards its
        // own forced-plane path against exactly this (`OutOrbitPitch = bForcePlaneThumbnail ?
        // 0.0f : ThumbnailInfo->OrbitPitch`, ThumbnailHelpers.cpp:443) and leaves the
        // caller-REQUESTED plane unguarded. So neither angle is applied to a plane; the asset's
        // own stored angles are kept, which is the framing that works, and the handler reports
        // that the request was not honoured. Yaw is ungated on both engine paths and has the
        // same exposure (azimuth 0 is edge on too), so azimuth is suppressed alongside
        // elevation rather than left as a second way to produce the same empty frame.
        bPlaneCameraAnglesIgnored =
            bResolvedShapeIsPlane && (Request.bHasElevation || Request.bHasAzimuth);

        // Sign conventions, derived from FThumbnailPreviewScene::CreateView: the camera
        // position works out to (-Zoom*cos(OrbitPitch)*sin(OrbitYaw), -Zoom*cos(OrbitPitch)*
        // cos(OrbitYaw), -Zoom*sin(OrbitPitch)). So a NEGATIVE OrbitPitch raises the camera
        // (elevation = -OrbitPitch), and the camera lands on +X when OrbitYaw is -90
        // (azimuth = -90 - OrbitYaw, matching camera.frame_actor's "azimuth 0 sits on +X,
        // increasing toward +Y"). The engine's own defaults (-11.25, -157.5) therefore read
        // as elevation 11.25 deg, azimuth 67.5 deg — the familiar three-quarter view.
        if (Request.bHasElevation && !bResolvedShapeIsPlane)
        {
            OrbitPitchProp = FindNumeric(Existing, OrbitPitchFieldName);
            if (!OrbitPitchProp)
            {
                OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                OutErrorMessage =
                    TEXT("SceneThumbnailInfo.OrbitPitch not found by reflection (engine layout drift?)");
                Restore();
                return;
            }
            void* ValuePtr = OrbitPitchProp->ContainerPtrToValuePtr<void>(Existing);
            SavedOrbitPitch =
                static_cast<float>(OrbitPitchProp->GetFloatingPointPropertyValue(ValuePtr));
            bSavedOrbitPitch = true;
            OrbitPitchProp->SetFloatingPointPropertyValue(
                ValuePtr, static_cast<double>(-Request.Elevation));
        }

        if (Request.bHasAzimuth && !bResolvedShapeIsPlane)
        {
            OrbitYawProp = FindNumeric(Existing, OrbitYawFieldName);
            if (!OrbitYawProp)
            {
                OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                OutErrorMessage =
                    TEXT("SceneThumbnailInfo.OrbitYaw not found by reflection (engine layout drift?)");
                Restore();
                return;
            }
            void* ValuePtr = OrbitYawProp->ContainerPtrToValuePtr<void>(Existing);
            SavedOrbitYaw = static_cast<float>(OrbitYawProp->GetFloatingPointPropertyValue(ValuePtr));
            bSavedOrbitYaw = true;
            OrbitYawProp->SetFloatingPointPropertyValue(
                ValuePtr, static_cast<double>(-90.0f - Request.Azimuth));
        }

        if (Request.bHasZoom)
        {
            OrbitZoomProp = FindNumeric(Existing, OrbitZoomFieldName);
            if (!OrbitZoomProp)
            {
                OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                OutErrorMessage =
                    TEXT("SceneThumbnailInfo.OrbitZoom not found by reflection (engine layout drift?)");
                Restore();
                return;
            }
            void* ValuePtr = OrbitZoomProp->ContainerPtrToValuePtr<void>(Existing);
            SavedOrbitZoom = static_cast<float>(OrbitZoomProp->GetFloatingPointPropertyValue(ValuePtr));
            bSavedOrbitZoom = true;
            OrbitZoomProp->SetFloatingPointPropertyValue(
                ValuePtr, static_cast<double>(Request.Zoom));
        }

        bValid = true;
    }

    void FScopedPreviewOverride::ApplyBaseMaterialShapeFlag(UObject* Asset)
    {
        UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Asset);
        if (!MaterialInterface)
        {
            return;
        }

        UMaterial* BaseMaterial = MaterialInterface->GetBaseMaterial();
        if (!BaseMaterial || BaseMaterial == Asset)
        {
            // A UMaterial is its own base, and its flag was set on the block above.
            return;
        }

        BaseThumbnailInfoProp = CastField<FObjectProperty>(
            BaseMaterial->GetClass()->FindPropertyByName(ThumbnailInfoFieldName));
        if (!BaseThumbnailInfoProp)
        {
            return;
        }
        BaseMaterialPtr = BaseMaterial;

        UObject* BaseInfo = BaseThumbnailInfoProp->GetObjectPropertyValue(
            BaseThumbnailInfoProp->ContainerPtrToValuePtr<void>(BaseMaterial));

        // A master whose thumbnail was never edited carries no ThumbnailInfo at all, and
        // ShouldForcePlanePreview then reads the class CDO — shared by every material and never
        // safe to write. Swap in a transient instance for the scope instead, exactly as the
        // named asset's path above does.
        if (!BaseInfo || !BaseInfo->GetClass()->FindPropertyByName(UserModifiedShapeFieldName))
        {
            UClass* InfoClass =
                FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.SceneThumbnailInfoWithPrimitive"));
            if (!InfoClass)
            {
                return;
            }

            UObject* NewInfo = NewObject<UObject>(BaseMaterial, InfoClass, NAME_None, RF_Transient);
            if (!NewInfo)
            {
                return;
            }

            if (BaseInfo)
            {
                CopyFloatField(BaseInfo, NewInfo, OrbitPitchFieldName);
                CopyFloatField(BaseInfo, NewInfo, OrbitYawFieldName);
                CopyFloatField(BaseInfo, NewInfo, OrbitZoomFieldName);
            }

            BasePreviousThumbnailInfo.Reset(BaseInfo);
            BaseThumbnailInfoProp->SetObjectPropertyValue(
                BaseThumbnailInfoProp->ContainerPtrToValuePtr<void>(BaseMaterial), NewInfo);
            bSwappedBaseThumbnailInfo = true;
            BaseInfo = NewInfo;
        }

        BaseActiveThumbnailInfo.Reset(BaseInfo);

        BaseUserModifiedShapeProp = CastField<FBoolProperty>(
            BaseInfo->GetClass()->FindPropertyByName(UserModifiedShapeFieldName));
        if (!BaseUserModifiedShapeProp)
        {
            return;
        }
        SavedBaseUserModifiedShape =
            BaseUserModifiedShapeProp->GetPropertyValue_InContainer(BaseInfo);
        bSavedBaseUserModifiedShape = true;
        BaseUserModifiedShapeProp->SetPropertyValue_InContainer(BaseInfo, true);
    }

    FScopedPreviewOverride::~FScopedPreviewOverride()
    {
        Restore();
    }

    void FScopedPreviewOverride::Restore()
    {
        UObject* Info = ActiveThumbnailInfo.Get();
        if (Info)
        {
            if (bSavedOrbitZoom && OrbitZoomProp)
            {
                OrbitZoomProp->SetFloatingPointPropertyValue(
                    OrbitZoomProp->ContainerPtrToValuePtr<void>(Info),
                    static_cast<double>(SavedOrbitZoom));
            }
            if (bSavedOrbitYaw && OrbitYawProp)
            {
                OrbitYawProp->SetFloatingPointPropertyValue(
                    OrbitYawProp->ContainerPtrToValuePtr<void>(Info),
                    static_cast<double>(SavedOrbitYaw));
            }
            if (bSavedOrbitPitch && OrbitPitchProp)
            {
                OrbitPitchProp->SetFloatingPointPropertyValue(
                    OrbitPitchProp->ContainerPtrToValuePtr<void>(Info),
                    static_cast<double>(SavedOrbitPitch));
            }
            if (bSavedUserModifiedShape && UserModifiedShapeProp)
            {
                UserModifiedShapeProp->SetPropertyValue_InContainer(Info, SavedUserModifiedShape);
            }
            if (bSavedPreviewMesh && PreviewMeshProp)
            {
                *PreviewMeshProp->ContainerPtrToValuePtr<FSoftObjectPath>(Info) = SavedPreviewMesh;
            }
            if (bSavedPrimitiveType && PrimitiveTypeProp)
            {
                PrimitiveTypeProp->SetIntPropertyValue(
                    PrimitiveTypeProp->ContainerPtrToValuePtr<void>(Info),
                    static_cast<int64>(SavedPrimitiveType));
            }
        }
        bSavedOrbitZoom = false;
        bSavedOrbitYaw = false;
        bSavedOrbitPitch = false;
        bSavedUserModifiedShape = false;
        bSavedPreviewMesh = false;
        bSavedPrimitiveType = false;

        if (bSwappedThumbnailInfo && ThumbnailInfoProp)
        {
            if (UObject* Asset = AssetPtr.Get())
            {
                ThumbnailInfoProp->SetObjectPropertyValue(
                    ThumbnailInfoProp->ContainerPtrToValuePtr<void>(Asset),
                    PreviousThumbnailInfo.Get());
            }
            bSwappedThumbnailInfo = false;
        }

        ActiveThumbnailInfo.Reset();
        PreviousThumbnailInfo.Reset();

        // The base material is a second asset this guard borrowed; it gets the same treatment,
        // or a material instance render would leave its master permanently shape-pinned.
        if (UObject* BaseInfo = BaseActiveThumbnailInfo.Get())
        {
            if (bSavedBaseUserModifiedShape && BaseUserModifiedShapeProp)
            {
                BaseUserModifiedShapeProp->SetPropertyValue_InContainer(
                    BaseInfo, SavedBaseUserModifiedShape);
            }
        }
        bSavedBaseUserModifiedShape = false;

        if (bSwappedBaseThumbnailInfo && BaseThumbnailInfoProp)
        {
            if (UObject* BaseMaterial = BaseMaterialPtr.Get())
            {
                BaseThumbnailInfoProp->SetObjectPropertyValue(
                    BaseThumbnailInfoProp->ContainerPtrToValuePtr<void>(BaseMaterial),
                    BasePreviousThumbnailInfo.Get());
            }
            bSwappedBaseThumbnailInfo = false;
        }

        BaseActiveThumbnailInfo.Reset();
        BasePreviousThumbnailInfo.Reset();
    }
}
