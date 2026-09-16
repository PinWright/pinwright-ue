// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
// FNumericProperty and the other FProperty subclasses held by pointer below. Reached only
// through the unity blob without this: the strict-include package build (-DisableUnity) sees
// FNumericProperty* as an undeclared type.
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

namespace PinWrightThumbnail
{
    // One caller's requested deviation from the asset's own stored thumbnail settings.
    // Every field is opt-in: an all-default request means "render exactly what the content
    // browser would render", which is the no-parameters behaviour of asset.generate_thumbnail.
    struct FPreviewOverrideRequest
    {
        // EThumbnailPrimType value (TPT_None=0, Sphere=1, Cube=2, Plane=3, Cylinder=4,
        // ShaderBall=5). "mesh" is expressed as TPT_None plus PreviewMesh, which is how the
        // engine's FMaterialThumbnailScene spells a custom preview shape.
        bool bHasPrimitive = false;
        uint8 PrimitiveType = 0;

        // Canonical object path of an already-LOADED UStaticMesh. It must be loaded because
        // the engine resolves this field with FSoftObjectPath::ResolveObject() and never
        // TryLoad(), so an unloaded mesh silently degrades the preview to a flat plane.
        FSoftObjectPath PreviewMesh;

        // Camera pose in the same vocabulary camera.frame_actor uses: azimuth is the
        // horizontal orbit angle in degrees (0 on +X, increasing toward +Y), elevation is
        // degrees above the horizon, zoom is a distance offset in world units added to the
        // automatic bounds-fit distance (negative moves closer).
        bool bHasAzimuth = false;
        float Azimuth = 0.0f;
        bool bHasElevation = false;
        float Elevation = 0.0f;
        bool bHasZoom = false;
        float Zoom = 0.0f;

        bool IsEmpty() const
        {
            return !bHasPrimitive && !bHasAzimuth && !bHasElevation && !bHasZoom;
        }
    };

    // Maps a caller-facing primitive name ("sphere" | "cube" | "plane" | "cylinder" |
    // "shaderBall" | "mesh", case-insensitive) onto its EThumbnailPrimType value. Returns
    // false for anything else. OutIsCustomMesh reports whether the name was "mesh", which
    // is the one spelling that additionally requires a primitiveMesh path.
    bool ParsePrimitiveName(const FString& Name, uint8& OutPrimitiveType, bool& OutIsCustomMesh);

    // Human-readable list of the accepted primitive names, for error messages.
    FString PrimitiveNameList();

    // Reports whether the engine will draw a flat, camera-facing plane for this asset whatever
    // PrimitiveType its ThumbnailInfo carries, and names the rule that forces it. This is
    // FMaterialThumbnailScene::SetMaterialInterface's own predicate: it computes
    // bForcePlaneThumbnail from GetBaseMaterial()->ShouldForcePlanePreview() and then discards
    // the requested shape with `bForcePlaneThumbnail ? TPT_Plane : ThumbnailInfo->PrimitiveType`.
    // Answer it before publishing a `primitive` field, or the response names a shape the pixels
    // do not show. Returns false for anything that is not a UMaterialInterface.
    bool WillForcePlanePreview(UObject* Asset, FString& OutReason);

    // Applies a FPreviewOverrideRequest to an asset's ThumbnailInfo for the lifetime of the
    // guard and restores every touched field in the destructor.
    //
    // Why a guard and not a plain setter: ThumbnailInfo is an Instanced UPROPERTY outered to
    // the asset, so it SAVES INTO THE ASSET'S PACKAGE — the editor's own thumbnail-edit UI
    // calls MarkPackageDirty() right after touching it. asset.generate_thumbnail is a
    // read-only preview verb, so a persistent write is out of bounds. The mutation window is
    // closed by construction: ThumbnailTools::RenderThumbnail with NeverFlush is synchronous
    // on the game thread, so nothing else observes the modified asset between construction
    // and destruction, and the package is never marked dirty. When the asset has no
    // ThumbnailInfo (or one that cannot carry a primitive), the guard swaps in an
    // RF_Transient instance for the scope and puts the original pointer back on exit —
    // transient so that even a save racing in from another editor path would serialize null
    // rather than persist the override.
    //
    // Every engine field is reached by reflection (FindPropertyByName + FProperty accessors)
    // rather than by including UnrealEd's MinimalAPI USceneThumbnailInfo* headers: those
    // classes export no member symbols, UStaticMesh::ThumbnailInfo is deprecated for direct
    // member access from 5.7, and a missing property is then a typed runtime error instead of
    // a link error.
    class FScopedPreviewOverride
    {
    public:
        // On failure sets OutErrorCode / OutErrorMessage and leaves the asset untouched;
        // check IsValid() before rendering. An empty request always succeeds and applies
        // nothing.
        FScopedPreviewOverride(UObject* Asset, const FPreviewOverrideRequest& Request,
            FString& OutErrorCode, FString& OutErrorMessage);
        ~FScopedPreviewOverride();

        FScopedPreviewOverride(const FScopedPreviewOverride&) = delete;
        FScopedPreviewOverride& operator=(const FScopedPreviewOverride&) = delete;

        bool IsValid() const { return bValid; }

        // True when the engine will draw a plane instead of the requested primitive. Resolved
        // inside this scope, after the guard has done what it can to defeat the defeatable
        // rules, so it is the shape the render will actually produce rather than the request.
        bool WasForcedToPlane() const { return bForcedToPlane; }
        const FString& GetForcePlaneReason() const { return ForcePlaneReason; }

        // True when `azimuth` / `elevation` were requested and NOT applied because the resolved
        // preview shape is the engine's thumbnail plane, which is legible from one direction
        // only. The asset's own stored angles were used instead.
        bool WerePlaneCameraAnglesIgnored() const { return bPlaneCameraAnglesIgnored; }

    private:
        void Restore();

        // Sets bUserModifiedShape on the BASE material's ThumbnailInfo for the scope. For a
        // material instance that is a different object from the one the primitive was written
        // to, and it is the one the engine's force-plane gate reads.
        void ApplyBaseMaterialShapeFlag(UObject* Asset);

        bool bValid = false;

        TWeakObjectPtr<UObject> AssetPtr;
        FObjectProperty* ThumbnailInfoProp = nullptr;

        // The object whose fields were edited. Held strongly because overwriting the asset's
        // ThumbnailInfo pointer can drop the only reference to the previous instance, and
        // losing it to a GC would turn this read-only verb into a destructive one.
        TStrongObjectPtr<UObject> ActiveThumbnailInfo;
        TStrongObjectPtr<UObject> PreviousThumbnailInfo;
        bool bSwappedThumbnailInfo = false;

        FNumericProperty* PrimitiveTypeProp = nullptr;
        uint8 SavedPrimitiveType = 0;
        bool bSavedPrimitiveType = false;

        FStructProperty* PreviewMeshProp = nullptr;
        FSoftObjectPath SavedPreviewMesh;
        bool bSavedPreviewMesh = false;

        FBoolProperty* UserModifiedShapeProp = nullptr;
        bool SavedUserModifiedShape = false;
        bool bSavedUserModifiedShape = false;

        FNumericProperty* OrbitPitchProp = nullptr;
        float SavedOrbitPitch = 0.0f;
        bool bSavedOrbitPitch = false;

        FNumericProperty* OrbitYawProp = nullptr;
        float SavedOrbitYaw = 0.0f;
        bool bSavedOrbitYaw = false;

        FNumericProperty* OrbitZoomProp = nullptr;
        float SavedOrbitZoom = 0.0f;
        bool bSavedOrbitZoom = false;

        // The base UMaterial of a material instance, borrowed the same way and restored on
        // every exit path. Only bUserModifiedShape is touched on it; the engine reads nothing
        // else off the base's ThumbnailInfo during a thumbnail render.
        TWeakObjectPtr<UObject> BaseMaterialPtr;
        FObjectProperty* BaseThumbnailInfoProp = nullptr;
        TStrongObjectPtr<UObject> BaseActiveThumbnailInfo;
        TStrongObjectPtr<UObject> BasePreviousThumbnailInfo;
        bool bSwappedBaseThumbnailInfo = false;
        FBoolProperty* BaseUserModifiedShapeProp = nullptr;
        bool SavedBaseUserModifiedShape = false;
        bool bSavedBaseUserModifiedShape = false;

        bool bForcedToPlane = false;
        FString ForcePlaneReason;
        bool bPlaneCameraAnglesIgnored = false;
    };
}
