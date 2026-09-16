// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// The preview-scene rig: the key light, the sky light and the backdrop of an asset-editor preview
// scene, applied for the life of ONE capture and put back on every exit path.
//
// WHY A SCOPED PARAMETER AND NOT A VERB. `editor.set_view_mode` writes viewport state
// PERSISTENTLY and never restores itself, so a diagnostic view taken that way leaks into every
// later capture and into what the user is looking at. The same objection applies with more force
// here, because the state a preview-scene rig touches is not even per-viewport: it is a
// PROCESS-WIDE UObject (`UAssetViewerSettings`) that every open asset editor in the session shares,
// and it is flushed to a COMMITTED project config file (`Config/DefaultEditor.ini`) by the
// destructor of any "Preview Scene Settings" details tab. So there is no `render.set_lighting`;
// there is one optional `previewScene` object on the capture verbs that own a preview scene.
//
// THREE LEVELS OF STATE, AND ONLY THE SHALLOWEST IS OBVIOUS. A capture that changes the key light
// must put back
//   (a) the light COMPONENTS on the scene,
//   (b) the shared `UAssetViewerSettings::Profiles` array, and
//   (c) the bytes of `Config/DefaultEditor.ini`.
// Level (b) is not bookkeeping. `FAdvancedPreviewScene::UpdateScene` compares
// `GetLightDirection()` against `Profile.DirectionalLightRotation` and, when they differ, writes
// the COMPONENT's rotation into the SHARED PROFILE
// (UE 5.8 Editor/AdvancedPreviewScene/Private/AdvancedPreviewScene.cpp:177-188). A component-only
// override therefore LAUNDERS ITSELF into the shared profile, which is then written to the
// committed config by the next details-tab teardown. That is why the restore order in
// FScopedPreviewSceneRig is components first, profile second, broadcast third and conditional --
// see the comment on the destructor.
//
// AND THE COMPARISON IS EXACT, SO IT FIRES ON ITS OWN. `GetLightDirection()` does not return a
// stored rotator: it derives one from the component's +X axis
// (Runtime/Engine/Private/PreviewScene.cpp:264-272), and `bLightDirChanged` is a bare
// `FRotator::operator!=`. The engine's own comment at :175 says as much ("the default profile
// light orientation might not match the LightingRigRotation"). So the write-back can fire on a
// scene nobody touched, which is the second reason the whole profile array is snapshotted rather
// than only the fields this rig writes.
#include "CoreMinimal.h"

// FPreviewSceneProfile -- held BY VALUE in the guard's snapshot, so the complete type is required
// rather than a forward declaration. Brings in the AdvancedPreviewScene module, which
// PinWright.Build.cs gains for exactly this file.
#include "AssetViewerSettings.h"

class FAdvancedPreviewScene;
class FEditorViewportClient;
class FJsonObject;
class FPreviewScene;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
struct FEngineShowFlags;

namespace PinWrightPreviewSceneRig
{
    // ---- arrival azimuth / elevation ----
    //
    // AZIMUTH AND ELEVATION ARE WHERE THE LIGHT ARRIVES FROM, not where it points, and the engine
    // default proves the conversion. `FPreviewScene::GetLightDirection()` returns the component's
    // +X axis (PreviewScene.cpp:264-272) and `ULightComponent::GetDirection()` is NEGATED at every
    // shading site (DirectionalLightComponent.cpp:396, :820) -- +X is the direction light TRAVELS.
    // So the arrival direction is -X, and
    //
    //     Rotation = (pitch = -elevation, yaw = azimuth - 180, roll = 0)
    //
    // The shipped default `FRotator(-40, -67.5, 0)` (PreviewScene.h:23, and the same value on
    // every FPreviewSceneProfile at AssetViewerSettings.h:62) is therefore arrival azimuth 112.5,
    // elevation 40. That number is corroborated independently by measurement rather than by trig:
    // docs/wiki-src/visual-review.model-rig.md:52 re-measured the lit band on a real capture set
    // and put the key "near azimuth 110", with the whole facing surface lit at 85/110/130 and a
    // black silhouette at -40 and -95. Two methods, 2.5 degrees apart.
    //
    // Both directions are published on every capture so no caller ever redoes this.
    FRotator ArrivalToLightRotation(double AzimuthDegrees, double ElevationDegrees);
    void     LightRotationToArrival(const FRotator& Rotation, double& OutAzimuthDegrees,
                                    double& OutElevationDegrees);

    // Default-constructed writes NOTHING -- the contract FExposurePin::Unset and
    // FViewModePin(bRequested=false) already carry.
    struct FPreviewSceneRigPin
    {
        bool bRequested = false;

        bool   bKeyAimProvided       = false;
        double KeyAzimuthDegrees     = 0.0;   // where the key ARRIVES from
        double KeyElevationDegrees   = 0.0;
        bool   bKeyIntensityProvided = false;
        double KeyIntensity          = 0.0;
        bool   bKeyColorProvided     = false;
        FColor KeyColor              = FColor::White;   // SetLightColor takes FColor, not FLinearColor

        bool   bSkyIntensityProvided = false;
        double SkyIntensity          = 0.0;

        bool bShowFloorProvided       = false;
        bool bShowFloor               = true;
        bool bShowEnvironmentProvided = false;
        bool bShowEnvironment         = true;

        bool WantsRig() const { return bRequested; }

        // The half of the rig that needs an FAdvancedPreviewScene rather than a bare
        // FPreviewScene. `key` and `sky` are ENGINE_API setters on FPreviewScene itself
        // (PreviewScene.h:104-110); floor and environment live on the profile.
        bool WantsAdvancedScene() const { return bShowFloorProvided || bShowEnvironmentProvided; }
    };

    // Every field READ BACK off the live scene. Nothing here is echoed from a request.
    struct FPreviewSceneRigReport
    {
        bool     bSceneAvailable = false;
        bool     bAdvancedScene  = false;
        // A bare, per-call FPreviewScene can still own the same floor and environment components
        // without consulting the process-wide UAssetViewerSettings profiles. This separates
        // "not an FAdvancedPreviewScene" from "floor/environment were not measured".
        bool     bBackdropVisibilityMeasured = false;
        FString  ProfileName;
        int32    ProfileIndex    = INDEX_NONE;

        FRotator KeyRotation     = FRotator::ZeroRotator;
        double   KeyAzimuthDegrees   = 0.0;
        double   KeyElevationDegrees = 0.0;
        double   KeyIntensity    = 0.0;
        FColor   KeyColor        = FColor::White;

        double   SkyIntensity    = 0.0;
        bool     bSkyVisible     = false;
        FString  SkyCubemapPath;

        bool     bShowFloor         = false;
        bool     bShowEnvironment   = false;
        bool     bRotateLightingRig = false;

        bool     bPostProcessingShowFlag = false;
        bool     bTonemapperShowFlag     = false;
        bool     bEyeAdaptationShowFlag  = false;
    };

    // Read the `previewScene` wire parameter off a payload root. Absent -> an unset pin with no
    // error. Present but unusable -> false plus ERR_INVALID_ARGUMENT and a message naming what is
    // wrong; a rig that silently half-applies produces a plausible picture under a lighting setup
    // nobody chose, which is exactly the failure this parameter exists to remove.
    //
    // An EMPTY `previewScene: {}` is refused rather than read as absent: an object that asks for
    // nothing is a caller mistake, and the same reasoning already governs
    // `exposure: {mode:"auto", ev100:5}`.
    //
    // Half an aim is refused too. `azimuth` without `elevation` (or the reverse) would silently
    // keep the other half of a rig the caller never measured, and the message names BOTH fields.
    bool ParsePreviewSceneRigPin(const TSharedPtr<FJsonObject>& Payload,
        FPreviewSceneRigPin& OutPin, FString& OutErrCode, FString& OutErrMsg);

    // Never writes. Safe on a client with no preview scene: returns bSceneAvailable=false.
    //
    // Takes the client by CONST reference per the frozen contract, and const_casts internally --
    // `FEditorViewportClient::GetPreviewScene()` and `FPreviewScene::GetLightDirection()` are both
    // non-const in UE 5.8 (EditorViewportClient.h:363, PreviewScene.h:104) although neither
    // mutates anything this function reaches. The cast is confined to this file so the six other
    // chunks coding against the frozen declaration are not made to care.
    FPreviewSceneRigReport MeasureRig(const FEditorViewportClient& Client);

    // Field-wise report equality used by both the single-capture and pose-set restore ledgers.
    bool RigReportsMatch(const FPreviewSceneRigReport& A, const FPreviewSceneRigReport& B);

    // Field-wise request verification shared by viewport-backed and transient preview captures.
    bool RigMatchesRequest(const FPreviewSceneRigPin& Pin,
        const FPreviewSceneRigReport& Drawn);

    // True when this viewport widget type is known to build an FAdvancedPreviewScene.
    // Exact-name allow-list, never a suffix match -- the rule CaptureSubject.cpp already follows,
    // and for the same reason: without RTTI a name-suffix match reaching a static_cast is
    // undefined behaviour, and stock UE 5.8 ships several SCompoundWidgets whose names end in
    // "Viewport".
    bool IsAdvancedPreviewViewport(const FEditorViewportClient& Client);

    // The scene's live profile, or null when its index is out of range.
    //
    // FAdvancedPreviewScene::GetCurrentProfile() is UE 5.6+; on 5.5 only the INDEX accessor is
    // public and DefaultSettings is protected. This reproduces the 5.6 body exactly
    // (AdvancedPreviewScene.cpp:236-242) using UAssetViewerSettings::Get(), which is what the
    // scene assigns DefaultSettings from in both of its constructors (:31, :110) - so the two
    // spellings resolve to the same FPreviewSceneProfile on every supported engine.
    FPreviewSceneProfile* CurrentProfile(FAdvancedPreviewScene& Scene);

    // ---- the sky and reflection captures, which ONLY an editor tick ever completes ----
    //
    // `FAdvancedPreviewScene` is an `FTickableEditorObject` (AdvancedPreviewScene.h:30) whose
    // `Tick` opens with `FPreviewScene::UpdateCaptureContents()` (:279-282), and that function is
    // the only sky-capture drain on this path -- its own comment names its three callers and all
    // three are Ticks (PreviewScene.cpp:245-253). `USkyLightComponent::SetCaptureIsDirty()`
    // recaptures nothing; it appends the component to a static queue
    // (SkyLightComponent.cpp:354-368) that only the drain empties, so a dirty sky light stays
    // stale until something ticks.
    //
    // A capture pumps Slate, invalidates and draws. It runs no editor frame, so it ran no drain:
    // every preview-scene shot used to be lit by whatever the last real editor frame left behind,
    // and the warm-up settle loop could not see it because its own pump cannot drive the work it
    // waits for -- a frame lit by a stale capture is not a frame in transition, it is a finished
    // frame of the wrong thing, and it converges immediately and legitimately.
    //
    // Called once per rig lifetime rather than per shot: the pose-set path owns one guard for the
    // whole set, so the drain runs when the lighting changes and not on every shutter.
    //
    // Returns whether the drain was actually DRIVEN. False when the scene has no world or the
    // world has no renderer scene -- both engine entry points early-out on that
    // (SkyLightComponent.cpp:922, ReflectionCaptureComponent.cpp:1192), so reporting true there
    // would claim work that did not happen. That false is what the response's
    // `previewScene.captureUpdated` and its warning publish rather than hide.
    //
    // DRIVEN IS NOT THE SAME AS COMPLETED, which is why there is a second output.
    // `UpdateSkyCaptureContentsArray` re-queues a component as `SLCS_CapturedButIncomplete` while
    // shaders, textures or meshes are still async-compiling and will not retry it sooner than 5
    // seconds later (SkyLightComponent.cpp:771-787, :837-860). So a drain can run in full and
    // leave the sky light exactly as stale as it found it -- the first capture after any asset
    // load is the ordinary way to reach that.
    //
    // `bOutCaptureIncomplete` is `USkyLightComponent::HasSkyCapturesToUpdate()` read AFTER the
    // drain. That predicate is PROCESS-WIDE, so it errs in one direction only: it can be true
    // because some other world's sky light is queued (a false alarm), and it cannot be false
    // while THIS world's is queued (never a false clear). It is reported separately rather than
    // folded into the return value for exactly that reason -- folding a process-wide reading into
    // a per-scene one would turn another window's pending capture into this capture's verdict.
    bool UpdatePreviewSceneCaptures(FPreviewScene* Scene, bool& bOutCaptureIncomplete);

    // Is any sky capture still queued, process-wide?
    //
    // `USkyLightComponent::HasSkyCapturesToUpdate()` is 5.8-only. Before it the same reading is the
    // two process-wide queues that predicate's body walks, which are ENGINE_API statics in the
    // component's PROTECTED section, so only a derived scope can name them -- see the definition.
    bool HasSkyCapturesToUpdate();

    // The rig this pin asks for is reachable on this client. False fills OutErrCode/OutErrMsg with
    // ERR_UNSUPPORTED_ASSET_EDITOR and names the field that cannot be honoured. Called BEFORE the
    // guard is constructed, because a guard constructor cannot refuse.
    bool CanApplyRig(const FEditorViewportClient& Client, const FPreviewSceneRigPin& Pin,
        FString& OutErrCode, FString& OutErrMsg);

    // ---- the shared profile array and the committed config file, as a value ----
    //
    // Split out as a plain value type with free functions for the same reason
    // FEditorSpriteShowFlags was: the snapshot-and-restore property is then assertable with NO
    // viewport, NO world and NO GPU. A capture test that cannot get a GPU takes a conditional-skip
    // path and reports success having asserted nothing (board ticket
    // B-test-skips-assertions-silently), so the mechanism that actually closes the two defects is
    // deliberately testable without one.
    struct FSharedProfileSnapshot
    {
        // The WHOLE array, copied by value, index-agnostic. It needs no downcast and no assumption
        // about which profile any scene is on, and it captures whatever the ENGINE mutates during
        // the capture as well as whatever this rig writes.
        TArray<FPreviewSceneProfile> Profiles;
        // MD5 of Config/DefaultEditor.ini at snapshot time. Empty when the file does not exist,
        // which compares equal to a later empty digest and so reports "unchanged" correctly.
        FString ConfigDigest;
        // False when UAssetViewerSettings::Get() returned null -- possible only before the
        // AdvancedPreviewScene module has initialised, and reported rather than silently treated
        // as "nothing to restore".
        bool bCaptured = false;
    };

    // The committed config file the shared profiles are flushed to, COMPUTED THE WAY THE ENGINE
    // COMPUTES IT and never hardcoded: the CDO of `/Script/AdvancedPreviewScene.SharedProfiles`
    // answers `UObject::GetDefaultConfigFilename()` (Obj.cpp:3958-3987), which resolves that
    // class's `ClassConfigName` (`Editor`, from `UCLASS(config = Editor, defaultconfig)`) against
    // `FPaths::SourceConfigDir()`. Reached by REFLECTION rather than by `USharedProfiles::
    // StaticClass()`, because that class carries no `*_API` macro and so cannot be linked from
    // another module. Falls back to the same printf the engine uses if the class is not loaded.
    FString PreviewSceneConfigFilePath();

    // MD5 of a file's bytes, as a lowercase hex string. Empty when the file does not exist or
    // cannot be read.
    FString DigestFile(const FString& AbsolutePath);

    FSharedProfileSnapshot CaptureSharedProfiles();

    // FIELD-WISE, NOT `operator==`. `FPreviewSceneProfile::operator==` compares ONLY `ProfileName`
    // (AssetViewerSettings.h:237-240), so `TArray::operator!=` over these profiles cannot see a
    // changed `bShowFloor`, a moved `DirectionalLightRotation` or anything else this file exists
    // to restore -- it would report "equal" for every mutation both defects produce. This walks
    // every reflected property instead, through `UScriptStruct::CompareScriptStruct`, whose
    // property-wise fallback runs because `FPreviewSceneProfile` declares no
    // `TStructOpsTypeTraits` and therefore does not carry `STRUCT_IdenticalNative`
    // (Class.cpp:3663-3694).
    bool SharedProfilesMatch(const TArray<FPreviewSceneProfile>& A,
                             const TArray<FPreviewSceneProfile>& B);

    // Put the snapshot back if anything differs, and broadcast ONLY if something was actually put
    // back. Returns true when the live array matches the snapshot on exit. `bOutRestoreWasNeeded`
    // says whether a write happened at all, which is the difference between "nothing mutated the
    // profiles" and "something did and we undid it" -- opposite facts about the same capture.
    //
    // THE BROADCAST IS CONDITIONAL ON PURPOSE. `OnAssetViewerSettingsChanged().Broadcast(NAME_None)`
    // drives a full four-way `UpdateScene` on EVERY live FAdvancedPreviewScene in the editor
    // (AdvancedPreviewScene.cpp:609, :630) -- a capture reaching into windows it does not own.
    // Doing it unconditionally would cost that for nothing on every capture in the session.
    bool RestoreSharedProfiles(const FSharedProfileSnapshot& Snapshot, bool& bOutRestoreWasNeeded);

    // Snapshot the process-wide profile array on construction and put it back on destruction,
    // with no rig and no viewport involved.
    //
    // Exported separately from FScopedPreviewSceneRig because the two defects it disarms are NOT
    // both inside a capture. Defect 1 (`SNiagaraSystemViewport::Construct` calling
    // `SetFloorVisibility(false)` with `bDirect` defaulted false, SNiagaraSystemViewport.cpp:872)
    // fires while the asset EDITOR is being opened, which happens in the subject resolver before
    // any capture util is reached. Wrapping that open is a one-line change in whichever file owns
    // it; this is the guard it needs.
    class FScopedSharedProfiles
    {
    public:
        FScopedSharedProfiles();
        ~FScopedSharedProfiles();

        FScopedSharedProfiles(const FScopedSharedProfiles&) = delete;
        FScopedSharedProfiles& operator=(const FScopedSharedProfiles&) = delete;

        const FSharedProfileSnapshot& GetSnapshot() const { return Snapshot; }
        bool WasRestored() const { return bRestored; }
        bool RestoreWasNeeded() const { return bRestoreWasNeeded; }

    private:
        FSharedProfileSnapshot Snapshot;
        bool bRestored = true;
        bool bRestoreWasNeeded = false;
    };

    class FScopedPreviewSceneRig
    {
    public:
        FScopedPreviewSceneRig(FEditorViewportClient& InClient, const FPreviewSceneRigPin& Pin);
        FScopedPreviewSceneRig(FPreviewScene& InScene, const FVector& SubjectBoundsOrigin,
            double SubjectBoundsRadius, const FPreviewSceneRigPin& Pin,
            FString& OutErrCode, FString& OutErrMsg);
        ~FScopedPreviewSceneRig();

        FScopedPreviewSceneRig(const FScopedPreviewSceneRig&) = delete;
        FScopedPreviewSceneRig& operator=(const FScopedPreviewSceneRig&) = delete;

        bool WasApplied() const { return bApplied; }
        // The sky/reflection capture drain ran for this scene. Asked on EVERY path, including the
        // omitted-parameter one: a capture that requested no rig is lit by the same never-drained
        // queue as one that did, so "nothing was requested" is not a reason to report nothing.
        bool CaptureContentsUpdated() const { return bCaptureContentsUpdated; }
        // A sky capture was STILL queued when the drain returned -- the engine defers an
        // incomplete capture while assets compile and retries no sooner than 5 s later. Only
        // meaningful alongside CaptureContentsUpdated(); see UpdatePreviewSceneCaptures for the
        // one direction this process-wide reading can be wrong in.
        bool CaptureContentsIncomplete() const { return bCaptureContentsIncomplete; }
        const FPreviewSceneRigReport& GetPreviousRig() const { return PreviousRig; }
        bool    ProfilesRestored()   const { return bProfilesRestored; }
        FString ConfigDigestAtEntry() const { return ConfigDigestBefore; }
        FString ConfigDigestAtExit()  const { return ConfigDigestAfter; }
        bool IsValid() const { return bInitialized; }
        FPreviewSceneRigReport Measure(const FEngineShowFlags& ShowFlags) const;

    private:
        FEditorViewportClient*           Client = nullptr;
        FPreviewScene*                   Scene = nullptr;
        UStaticMeshComponent*            FloorComponent = nullptr;
        UStaticMeshComponent*            EnvironmentComponent = nullptr;
        UMaterialInstanceDynamic*        EnvironmentMaterial = nullptr;
        FString                          SkyCubemapPath;
        FPreviewSceneRigReport           PreviousRig;
        TArray<FPreviewSceneProfile>     ProfilesBefore;   // whole array, index-agnostic
        FString                          ConfigDigestBefore;
        FString                          ConfigDigestAfter;
        bool                             bApplied          = false;
        bool                             bCaptureContentsUpdated = false;
        bool                             bCaptureContentsIncomplete = false;
        bool                             bProfilesRestored = true;
        bool                             bDisposable       = false;
        bool                             bInitialized      = false;
        // Whether ProfilesBefore is a real snapshot rather than a default-constructed empty
        // array. NOT derivable from ProfilesBefore.Num(): assigning an empty array back over the
        // live one would DELETE every preview-scene profile in the editor, so "we never got a
        // snapshot" and "the snapshot was empty" must not be the same state.
        bool                             bProfilesCaptured = false;
    };
}
