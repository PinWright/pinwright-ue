// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/PreviewSceneRig.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"

#include "AdvancedPreviewScene.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "SEditorViewport.h"
#include "ShowFlags.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace PinWrightPreviewSceneRig
{
namespace
{
    // Prefixed for the same reason every helper under Handlers/Render is: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.
    constexpr double RigMaxElevationDegrees = 90.0;
    // Intensity is a physical quantity on both lights (lux on the directional light,
    // ULightComponentBase::Intensity; a unitless scale on the sky light). Negative has no
    // rendering meaning and is a typo, not a request.
    constexpr double RigMaxIntensity = 1.0e6;

    // ---- the exact-name allow-list of viewport widgets that build an FAdvancedPreviewScene ----
    //
    // WHY A NAME LIST AND NOT A CAST. The plugin is built with RTTI off, so there is no
    // dynamic_cast, and FPreviewScene has no virtual "am I advanced" hook. A static_cast from
    // FPreviewScene* to FAdvancedPreviewScene* is only defined when the object really is one, so
    // the question has to be answered some other way. This is the same rule CaptureSubject.cpp
    // already follows for SEditorViewport, and for the same reason: stock UE 5.8 ships several
    // SCompoundWidgets whose names end in "Viewport", and a suffix match reaching a cast is
    // undefined behaviour.
    //
    // THE CAST IS A NO-OP ADJUSTMENT FOR EVERY ENTRY. FAdvancedPreviewScene derives
    // `: public FPreviewScene, public FTickableEditorObject` (AdvancedPreviewScene.h:30), so
    // FPreviewScene is the PRIMARY base and the pointer value is unchanged. Persona qualifies
    // through one more link: `IPersonaPreviewScene : public FAdvancedPreviewScene`
    // (Editor/Persona/Public/IPersonaPreviewScene.h:71), single inheritance, same primary chain.
    struct FAdvancedPreviewViewportEntry
    {
        const TCHAR* TypeName;
        // Where the ": public ..." chain and the FAdvancedPreviewScene member were read in UE 5.8.
        // These classes all live in Private/ engine headers a plugin cannot include, so the
        // citation IS the proof -- there is no compile-time half available for any of them.
        const TCHAR* Evidence;
    };

    const FAdvancedPreviewViewportEntry GAdvancedPreviewViewportAllowList[] =
    {
        // render.capture_asset_preview's viewport. SNew(SStaticMeshEditorViewport) makes the
        // stored type name exact.
        { TEXT("SStaticMeshEditorViewport"),
          TEXT("Editor/StaticMeshEditor/Private/SStaticMeshEditorViewport.h:238 "
               "(TSharedPtr<FAdvancedPreviewScene> PreviewScene), handed to the client at "
               "SStaticMeshEditorViewport.cpp:902") },

        // The Persona family (SkeletalMeshEditor / AnimationEditor / SkeletonEditor /
        // AnimationBlueprintEditor). Its scene is an IPersonaPreviewScene, which IS an
        // FAdvancedPreviewScene.
        { TEXT("SAnimationEditorViewport"),
          TEXT("Editor/Persona/Private/SAnimationEditorViewport.h:123 "
               "(TWeakPtr<IPersonaPreviewScene>), and IPersonaPreviewScene.h:71 "
               "(: public FAdvancedPreviewScene)") },

        // THE ENTRY A SUFFIX RULE COULD NEVER MATCH, and the same one CaptureSubject.cpp calls
        // out: the widget is SNew(SNiagaraSystemViewport, ...), so the type name does not end in
        // "EditorViewport".
        { TEXT("SNiagaraSystemViewport"),
          TEXT("Plugins/FX/Niagara/Source/NiagaraEditor/Private/Widgets/SNiagaraSystemViewport.h:145 "
               "(TSharedPtr<FAdvancedPreviewScene> AdvancedPreviewScene), dereferenced into the "
               "client at SNiagaraSystemViewport.cpp:1294") },

        // Not reachable by any verb in this plugin today, and enrolled anyway because it is the
        // one asset editor whose preview scene a materials review would want the rig for, and its
        // citation was read in the same pass as the others.
        { TEXT("SMaterialEditor3DPreviewViewport"),
          TEXT("Editor/MaterialEditor/Private/SMaterialEditorViewport.h:161 "
               "(TSharedPtr<FAdvancedPreviewScene> AdvancedPreviewScene)") },

        // Niagara's scalability-baseline viewport. Same module, same shape, and it is the second
        // caller of SetFloorVisibility(false) in that file (SNiagaraSystemViewport.cpp:1611), so
        // it arms Defect 1 exactly as the system viewport does.
        { TEXT("SNiagaraBaselineViewport"),
          TEXT("Plugins/FX/Niagara/Source/NiagaraEditor/Private/Widgets/SNiagaraSystemViewport.h:207 "
               "(TSharedPtr<FAdvancedPreviewScene> AdvancedPreviewScene)") }
    };

    bool RigIsAllowListedAdvancedViewportType(const FString& TypeName)
    {
        for (const FAdvancedPreviewViewportEntry& Entry : GAdvancedPreviewViewportAllowList)
        {
            if (TypeName.Equals(Entry.TypeName, ESearchCase::CaseSensitive))
            {
                return true;
            }
        }
        return false;
    }

    // The advanced scene behind a client, or null. The ONE place the static_cast happens, gated by
    // the allow-list above and by nothing else.
    FAdvancedPreviewScene* RigAdvancedSceneFor(FEditorViewportClient& Client)
    {
        FPreviewScene* Scene = Client.GetPreviewScene();
        if (!Scene)
        {
            return nullptr;
        }
        if (!IsAdvancedPreviewViewport(Client))
        {
            return nullptr;
        }
        return static_cast<FAdvancedPreviewScene*>(Scene);
    }

    // `#RRGGBB`, `#RRGGBBAA`, `#RGB`, `#RGBA`, with or without the leading `#`. Validated HERE
    // rather than leaning on FColor::FromHex, which returns opaque BLACK for anything it cannot
    // read (Color.cpp) -- indistinguishable from a caller who genuinely asked for black, and so a
    // silent way to light a subject with a colour nobody chose.
    bool RigParseHexColor(const FString& Text, FColor& OutColor)
    {
        const int32 Start = (!Text.IsEmpty() && Text[0] == TCHAR('#')) ? 1 : 0;
        const int32 Digits = Text.Len() - Start;
        if (Digits != 3 && Digits != 4 && Digits != 6 && Digits != 8)
        {
            return false;
        }
        for (int32 Index = Start; Index < Text.Len(); ++Index)
        {
            if (!FChar::IsHexDigit(Text[Index]))
            {
                return false;
            }
        }
        OutColor = FColor::FromHex(Text);
        return true;
    }

    // The reflected struct whose every property SharedProfilesMatch walks. UHT exports the
    // accessor -- `ADVANCEDPREVIEWSCENE_API UScriptStruct* Z_Construct_UScriptStruct_
    // FPreviewSceneProfile(ETypeConstructPhase)` in the generated header, which the inline
    // StaticStruct() forwards to -- so this links across the module boundary. Wrapped in one
    // function so the null case is handled in one place rather than at each use.
    UScriptStruct* RigProfileStruct()
    {
        return FPreviewSceneProfile::StaticStruct();
    }
}

// ---------------------------------------------------------------------------------------------
// Arrival azimuth / elevation
// ---------------------------------------------------------------------------------------------

FRotator ArrivalToLightRotation(double AzimuthDegrees, double ElevationDegrees)
{
    // Yaw is normalised into [-180, 180] so the returned rotator is the one FRotator::operator==
    // will match against a value read back from GetLightDirection(), which derives its yaw from a
    // vector and is therefore always normalised.
    return FRotator(-ElevationDegrees, FRotator::NormalizeAxis(AzimuthDegrees - 180.0), 0.0);
}

void LightRotationToArrival(const FRotator& Rotation, double& OutAzimuthDegrees,
    double& OutElevationDegrees)
{
    OutElevationDegrees = -static_cast<double>(Rotation.Pitch);
    double Azimuth = FMath::Fmod(static_cast<double>(Rotation.Yaw) + 180.0, 360.0);
    if (Azimuth < 0.0)
    {
        Azimuth += 360.0;
    }
    OutAzimuthDegrees = Azimuth;
}

// ---------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------

bool ParsePreviewSceneRigPin(const TSharedPtr<FJsonObject>& Payload,
    FPreviewSceneRigPin& OutPin, FString& OutErrCode, FString& OutErrMsg)
{
    OutPin = FPreviewSceneRigPin();
    OutErrCode.Reset();
    OutErrMsg.Reset();

    if (!Payload.IsValid() || !Payload->HasField(TEXT("previewScene")))
    {
        // The omitted-parameter path. Nothing is read and nothing will be written.
        return true;
    }

    const TSharedPtr<FJsonObject>* RigObject = nullptr;
    if (!Payload->TryGetObjectField(TEXT("previewScene"), RigObject) || !RigObject ||
        !RigObject->IsValid())
    {
        OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrMsg = TEXT("previewScene must be an object, e.g. "
                         "{\"key\":{\"azimuth\":110,\"elevation\":40,\"intensity\":4.0}, "
                         "\"sky\":{\"intensity\":2.0}}. Omit the field entirely to capture under "
                         "whatever rig the preview scene already has.");
        return false;
    }
    const TSharedPtr<FJsonObject>& Rig = *RigObject;

    // ---- key ----
    const TSharedPtr<FJsonObject>* KeyObject = nullptr;
    if (Rig->HasField(TEXT("key")))
    {
        if (!Rig->TryGetObjectField(TEXT("key"), KeyObject) || !KeyObject || !KeyObject->IsValid())
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("previewScene.key must be an object: "
                             "{azimuth, elevation, intensity, color}, every field optional.");
            return false;
        }
        const TSharedPtr<FJsonObject>& Key = *KeyObject;

        const bool bHasAzimuth = Key->HasField(TEXT("azimuth"));
        const bool bHasElevation = Key->HasField(TEXT("elevation"));
        if (bHasAzimuth != bHasElevation)
        {
            // HALF AN AIM IS REFUSED. Applying one and silently keeping the other half of a rig
            // the caller never measured produces a lit frame under a direction nobody chose, and
            // no field in the response could tell it from an aim that was honoured. The message
            // names BOTH fields so a caller who typed one knows what the pair is.
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = FString::Printf(
                TEXT("previewScene.key needs BOTH azimuth and elevation or neither; this request "
                     "supplied %s without %s. They are the two halves of one arrival direction -- "
                     "azimuth is the compass bearing the key light ARRIVES from and elevation is "
                     "its height above the horizon -- so applying one alone would aim the key "
                     "somewhere the caller never measured."),
                bHasAzimuth ? TEXT("azimuth") : TEXT("elevation"),
                bHasAzimuth ? TEXT("elevation") : TEXT("azimuth"));
            return false;
        }
        if (bHasAzimuth && bHasElevation)
        {
            double Azimuth = 0.0;
            double Elevation = 0.0;
            if (!Key->TryGetNumberField(TEXT("azimuth"), Azimuth) ||
                !Key->TryGetNumberField(TEXT("elevation"), Elevation))
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = TEXT("previewScene.key.azimuth and previewScene.key.elevation must "
                                 "both be numbers, in degrees.");
                return false;
            }
            if (Elevation < -RigMaxElevationDegrees || Elevation > RigMaxElevationDegrees)
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = FString::Printf(
                    TEXT("previewScene.key.elevation must be in [-90, 90] degrees; got %.4f. "
                         "Elevation is height above the horizon, so 90 is straight down from "
                         "overhead and -90 is straight up from below; a bearing belongs in "
                         "azimuth, which takes any angle."),
                    Elevation);
                return false;
            }
            OutPin.bKeyAimProvided = true;
            OutPin.KeyAzimuthDegrees = Azimuth;
            OutPin.KeyElevationDegrees = Elevation;
        }

        if (Key->HasField(TEXT("intensity")))
        {
            double Intensity = 0.0;
            if (!Key->TryGetNumberField(TEXT("intensity"), Intensity) ||
                Intensity < 0.0 || Intensity > RigMaxIntensity)
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = FString::Printf(
                    TEXT("previewScene.key.intensity must be a number in [0, %g]. The engine's "
                         "shipped preview profiles use 1.0 (Epic Headquarters / Grey Wireframe) "
                         "and 4.0 (Grey Ambient)."),
                    RigMaxIntensity);
                return false;
            }
            OutPin.bKeyIntensityProvided = true;
            OutPin.KeyIntensity = Intensity;
        }

        if (Key->HasField(TEXT("color")))
        {
            FString ColorText;
            FColor Color = FColor::White;
            if (!Key->TryGetStringField(TEXT("color"), ColorText) ||
                !RigParseHexColor(ColorText, Color))
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = TEXT("previewScene.key.color must be a hex string -- \"#RRGGBB\", "
                                 "\"#RRGGBBAA\", \"#RGB\" or \"#RGBA\", with or without the "
                                 "leading '#'. It is refused rather than defaulted because the "
                                 "engine's own FColor::FromHex returns opaque BLACK for anything "
                                 "it cannot read, which would light the subject in a colour "
                                 "nobody asked for and report success.");
                return false;
            }
            OutPin.bKeyColorProvided = true;
            OutPin.KeyColor = Color;
        }
    }

    // ---- sky ----
    const TSharedPtr<FJsonObject>* SkyObject = nullptr;
    if (Rig->HasField(TEXT("sky")))
    {
        if (!Rig->TryGetObjectField(TEXT("sky"), SkyObject) || !SkyObject || !SkyObject->IsValid())
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("previewScene.sky must be an object: {intensity}.");
            return false;
        }
        const TSharedPtr<FJsonObject>& Sky = *SkyObject;
        if (Sky->HasField(TEXT("intensity")))
        {
            double Intensity = 0.0;
            if (!Sky->TryGetNumberField(TEXT("intensity"), Intensity) ||
                Intensity < 0.0 || Intensity > RigMaxIntensity)
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = FString::Printf(
                    TEXT("previewScene.sky.intensity must be a number in [0, %g]. The engine's "
                         "shipped preview profiles use 1.0 and 2.0 (Grey Ambient)."),
                    RigMaxIntensity);
                return false;
            }
            OutPin.bSkyIntensityProvided = true;
            OutPin.SkyIntensity = Intensity;
        }
    }

    // ---- floor / environment ----
    if (Rig->HasField(TEXT("showFloor")))
    {
        bool bValue = true;
        if (!Rig->TryGetBoolField(TEXT("showFloor"), bValue))
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("previewScene.showFloor must be a boolean.");
            return false;
        }
        OutPin.bShowFloorProvided = true;
        OutPin.bShowFloor = bValue;
    }
    if (Rig->HasField(TEXT("showEnvironment")))
    {
        bool bValue = true;
        if (!Rig->TryGetBoolField(TEXT("showEnvironment"), bValue))
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("previewScene.showEnvironment must be a boolean.");
            return false;
        }
        OutPin.bShowEnvironmentProvided = true;
        OutPin.bShowEnvironment = bValue;
    }

    const bool bAnything = OutPin.bKeyAimProvided || OutPin.bKeyIntensityProvided ||
        OutPin.bKeyColorProvided || OutPin.bSkyIntensityProvided ||
        OutPin.bShowFloorProvided || OutPin.bShowEnvironmentProvided;
    if (!bAnything)
    {
        // AN EMPTY BLOCK IS A MISTAKE, NOT A NO-OP. Reading `previewScene: {}` as "absent" would
        // return a frame drawn under whatever rig happened to be loaded while the caller believes
        // they set one -- the same reasoning that already refuses
        // `exposure: {mode:"auto", ev100:5}` instead of half-applying it.
        OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrMsg = TEXT("previewScene was supplied but asks for nothing. Set at least one of "
                         "key.azimuth+key.elevation, key.intensity, key.color, sky.intensity, "
                         "showFloor or showEnvironment -- or omit previewScene entirely to "
                         "capture under the rig the preview scene already has.");
        return false;
    }

    OutPin.bRequested = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------------------------

bool IsAdvancedPreviewViewport(const FEditorViewportClient& Client)
{
    const TSharedPtr<SEditorViewport> Widget = Client.GetEditorViewportWidget();
    if (!Widget.IsValid())
    {
        return false;
    }
    return RigIsAllowListedAdvancedViewportType(Widget->GetTypeAsString());
}

FPreviewSceneProfile* CurrentProfile(FAdvancedPreviewScene& Scene)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    return Scene.GetCurrentProfile();
#else
    UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
    const int32 Index = Scene.GetCurrentProfileIndex();
    return (Settings && Settings->Profiles.IsValidIndex(Index)) ? &Settings->Profiles[Index] : nullptr;
#endif
}

#if UE_VERSION_OLDER_THAN(5, 8, 0)
namespace
{
    // UE 5.8 publishes the queue reading as USkyLightComponent::HasSkyCapturesToUpdate(). Before
    // it there is no accessor at all: the two queues its body walks are ENGINE_API statics -- so
    // they link -- sitting in the component's protected section, and a derived scope is the one
    // way C++ has to name a protected static. Nothing is ever constructed from this type; it
    // exists only to reach the two symbols.
    struct FSkyCaptureQueueProbe : public USkyLightComponent
    {
        static bool AnyQueued()
        {
            return SkyCapturesToUpdate.Num() > 0 || SkyCapturesToUpdateBlendDestinations.Num() > 0;
        }
    };
}
#endif

bool HasSkyCapturesToUpdate()
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    return USkyLightComponent::HasSkyCapturesToUpdate();
#else
    return FSkyCaptureQueueProbe::AnyQueued();
#endif
}

bool UpdatePreviewSceneCaptures(FPreviewScene* Scene, bool& bOutCaptureIncomplete)
{
    bOutCaptureIncomplete = false;
    if (!Scene)
    {
        return false;
    }
    UWorld* World = Scene->GetWorld();
    if (!World || !World->Scene)
    {
        // Both engine entry points test exactly this and do nothing without it, so calling them
        // here would return a drain that never ran.
        return false;
    }

    Scene->UpdateCaptureContents();
    // The recapture ends in MarkRenderStateDirty, which only QUEUES the sky light's proxy rebuild
    // for the next end-of-frame flush. The engine's own reflection path drains that list in-line
    // for the same reason (ReflectionCaptureComponent.cpp:1235), and no editor frame runs between
    // here and the draw that reads these pixels.
    World->SendAllEndOfFrameUpdates();
    // The convolution itself is render-thread work the engine deliberately does not block on
    // (FInitSkyProxy's comment, SkyLightComponent.cpp:288-290). Blocking here is what makes the
    // FIRST frame this capture draws the one lit by the new capture rather than a frame or two
    // later -- and on a capture call there is no later frame.
    FlushRenderingCommands();

    // Read AFTER the drain, because a drain that ran in full can still leave the component
    // queued: the engine defers an incomplete capture while assets compile and will not retry it
    // for 5 s (SkyLightComponent.cpp:771-787, :837-860). Nothing here can shorten that wait, so
    // it is measured and published instead of waited on.
    bOutCaptureIncomplete = HasSkyCapturesToUpdate();
    return true;
}

static FPreviewSceneRigReport MeasureSceneRigValues(FPreviewScene& Scene,
    const FEngineShowFlags& ShowFlags)
{
    FPreviewSceneRigReport Report;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    Report.bSceneAvailable = Scene.IsInitialized();
#else
    // FPreviewScene::IsInitialized arrived in 5.7 and is exactly this test (PreviewScene.h).
    Report.bSceneAvailable = Scene.GetWorld() != nullptr;
#endif
    Report.bPostProcessingShowFlag = ShowFlags.PostProcessing != 0;
    Report.bTonemapperShowFlag = ShowFlags.Tonemapper != 0;
    Report.bEyeAdaptationShowFlag = ShowFlags.EyeAdaptation != 0;
    if (Scene.DirectionalLight)
    {
        Report.KeyRotation = Scene.GetLightDirection();
        LightRotationToArrival(Report.KeyRotation, Report.KeyAzimuthDegrees,
            Report.KeyElevationDegrees);
        Report.KeyIntensity = static_cast<double>(Scene.DirectionalLight->Intensity);
        Report.KeyColor = Scene.DirectionalLight->LightColor;
    }
    if (Scene.SkyLight)
    {
        Report.SkyIntensity = static_cast<double>(Scene.SkyLight->Intensity);
        Report.bSkyVisible = Scene.SkyLight->GetVisibleFlag();
        if (Scene.SkyLight->Cubemap)
        {
            Report.SkyCubemapPath = Scene.SkyLight->Cubemap->GetPathName();
        }
    }
    return Report;
}

static void ApplySceneRigValues(FPreviewScene& Scene, const FPreviewSceneRigPin& Pin,
    UStaticMeshComponent* FloorComponent = nullptr,
    UStaticMeshComponent* EnvironmentComponent = nullptr,
    UMaterialInstanceDynamic* EnvironmentMaterial = nullptr)
{
    if (Pin.bKeyAimProvided)
    {
        Scene.SetLightDirection(ArrivalToLightRotation(
            Pin.KeyAzimuthDegrees, Pin.KeyElevationDegrees));
    }
    if (Pin.bKeyIntensityProvided)
    {
        Scene.SetLightBrightness(static_cast<float>(Pin.KeyIntensity));
    }
    if (Pin.bKeyColorProvided)
    {
        Scene.SetLightColor(Pin.KeyColor);
    }
    if (Pin.bSkyIntensityProvided)
    {
        Scene.SetSkyBrightness(static_cast<float>(Pin.SkyIntensity));
        if (EnvironmentMaterial)
        {
            EnvironmentMaterial->SetScalarParameterValue(
                TEXT("Intensity"), static_cast<float>(Pin.SkyIntensity));
        }
    }
    if (Pin.bShowFloorProvided && FloorComponent)
    {
        FloorComponent->SetVisibility(Pin.bShowFloor, true);
    }
    if (Pin.bShowEnvironmentProvided && EnvironmentComponent)
    {
        EnvironmentComponent->SetVisibility(Pin.bShowEnvironment, true);
    }
}

static void RestoreSceneRigValues(FPreviewScene& Scene,
    const FPreviewSceneRigReport& Previous,
    UStaticMeshComponent* FloorComponent = nullptr,
    UStaticMeshComponent* EnvironmentComponent = nullptr,
    UMaterialInstanceDynamic* EnvironmentMaterial = nullptr)
{
    if (!Previous.bSceneAvailable)
    {
        return;
    }
    Scene.SetLightDirection(Previous.KeyRotation);
    Scene.SetLightBrightness(static_cast<float>(Previous.KeyIntensity));
    Scene.SetLightColor(Previous.KeyColor);
    Scene.SetSkyBrightness(static_cast<float>(Previous.SkyIntensity));
    if (EnvironmentMaterial)
    {
        EnvironmentMaterial->SetScalarParameterValue(
            TEXT("Intensity"), static_cast<float>(Previous.SkyIntensity));
    }
    if (Previous.bBackdropVisibilityMeasured && FloorComponent)
    {
        FloorComponent->SetVisibility(Previous.bShowFloor, true);
    }
    if (Previous.bBackdropVisibilityMeasured && EnvironmentComponent)
    {
        EnvironmentComponent->SetVisibility(Previous.bShowEnvironment, true);
    }
}

FPreviewSceneRigReport MeasureRig(const FEditorViewportClient& ConstClient)
{
    // const_cast confined to this file -- see the header. Nothing below writes.
    FEditorViewportClient& Client = const_cast<FEditorViewportClient&>(ConstClient);

    FPreviewScene* Scene = Client.GetPreviewScene();
    if (!Scene)
    {
        // Every level-viewport verb lands here. bSceneAvailable stays false and the serializer
        // emits that and nothing else.
        FPreviewSceneRigReport Empty;
        Empty.bPostProcessingShowFlag = Client.EngineShowFlags.PostProcessing != 0;
        Empty.bTonemapperShowFlag = Client.EngineShowFlags.Tonemapper != 0;
        Empty.bEyeAdaptationShowFlag = Client.EngineShowFlags.EyeAdaptation != 0;
        return Empty;
    }
    FPreviewSceneRigReport Report = MeasureSceneRigValues(*Scene, Client.EngineShowFlags);

    if (FAdvancedPreviewScene* Advanced = RigAdvancedSceneFor(Client))
    {
        Report.bAdvancedScene = true;
        Report.ProfileIndex = Advanced->GetCurrentProfileIndex();
        if (const FPreviewSceneProfile* Profile = CurrentProfile(*Advanced))
        {
            Report.ProfileName = Profile->ProfileName;
            Report.bShowFloor = Profile->bShowFloor;
            Report.bShowEnvironment = Profile->bShowEnvironment;
            // A capture taken while this is true is NOT reproducible: FAdvancedPreviewScene::Tick
            // re-aims the key every frame and writes the new rotation into the shared profile
            // (AdvancedPreviewScene.cpp:285-294). No restore can fix that, which is exactly why
            // the field is published rather than corrected.
            Report.bRotateLightingRig = Profile->bRotateLightingRig;
        }
    }
    return Report;
}

bool RigReportsMatch(const FPreviewSceneRigReport& A, const FPreviewSceneRigReport& B)
{
    return A.bSceneAvailable == B.bSceneAvailable
        && A.bAdvancedScene == B.bAdvancedScene
        && A.bBackdropVisibilityMeasured == B.bBackdropVisibilityMeasured
        && A.ProfileName == B.ProfileName
        && A.ProfileIndex == B.ProfileIndex
        && A.KeyRotation.Equals(B.KeyRotation, UE_KINDA_SMALL_NUMBER)
        && FMath::IsNearlyEqual(A.KeyIntensity, B.KeyIntensity, UE_KINDA_SMALL_NUMBER)
        && A.KeyColor == B.KeyColor
        && FMath::IsNearlyEqual(A.SkyIntensity, B.SkyIntensity, UE_KINDA_SMALL_NUMBER)
        && A.bSkyVisible == B.bSkyVisible
        && A.SkyCubemapPath == B.SkyCubemapPath
        && A.bShowFloor == B.bShowFloor
        && A.bShowEnvironment == B.bShowEnvironment
        && A.bRotateLightingRig == B.bRotateLightingRig
        && A.bPostProcessingShowFlag == B.bPostProcessingShowFlag
        && A.bTonemapperShowFlag == B.bTonemapperShowFlag
        && A.bEyeAdaptationShowFlag == B.bEyeAdaptationShowFlag;
}

bool RigMatchesRequest(const FPreviewSceneRigPin& Pin,
    const FPreviewSceneRigReport& Drawn)
{
    if (!Drawn.bSceneAvailable)
    {
        return false;
    }
    const FRotator Wanted = ArrivalToLightRotation(
        Pin.KeyAzimuthDegrees, Pin.KeyElevationDegrees);
    return (!Pin.bKeyAimProvided || Drawn.KeyRotation.Equals(Wanted, 0.01))
        && (!Pin.bKeyIntensityProvided
            || FMath::IsNearlyEqual(Drawn.KeyIntensity, Pin.KeyIntensity, 0.001))
        && (!Pin.bKeyColorProvided || Drawn.KeyColor == Pin.KeyColor)
        && (!Pin.bSkyIntensityProvided
            || FMath::IsNearlyEqual(Drawn.SkyIntensity, Pin.SkyIntensity, 0.001))
        && (!Pin.bShowFloorProvided || Drawn.bShowFloor == Pin.bShowFloor)
        && (!Pin.bShowEnvironmentProvided
            || Drawn.bShowEnvironment == Pin.bShowEnvironment);
}

FScopedPreviewSceneRig::FScopedPreviewSceneRig(FPreviewScene& InScene,
    const FVector& SubjectBoundsOrigin, double SubjectBoundsRadius,
    const FPreviewSceneRigPin& Pin, FString& OutErrCode, FString& OutErrMsg)
    : Scene(&InScene)
    , bDisposable(true)
{
    OutErrCode.Reset();
    OutErrMsg.Reset();

    FPreviewSceneProfile Defaults;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    Defaults.LoadProfileObjects();

    UStaticMesh* FloorMesh = Defaults.GetEnvironmentFloorMesh();
    UMaterialInterface* FloorMaterial = Defaults.GetEnvironmentFloorMaterial();
    const FRotator FloorRotation = Defaults.EnvironmentFloorRotation;
#else
    // Before UE 5.8 the profile carried no floor asset configuration. The advanced preview scene
    // hardcoded this mesh, left it on its own material, and never rotated it
    // (AdvancedPreviewScene.cpp), so reproduce that here.
    UStaticMesh* FloorMesh = LoadObject<UStaticMesh>(nullptr,
        TEXT("/Engine/EditorMeshes/AssetViewer/Floor_Mesh.Floor_Mesh"));
    UMaterialInterface* FloorMaterial = FloorMesh ? FloorMesh->GetMaterial(0) : nullptr;
    const FRotator FloorRotation = FRotator::ZeroRotator;
    // A default-constructed profile leaves EnvironmentCubeMap null and keeps only the path;
    // pre-5.8 there is no LoadProfileObjects, so resolve the cube map through its own loader.
    Defaults.LoadEnvironmentMap();
#endif
    UTextureCube* EnvironmentTexture = Defaults.EnvironmentCubeMap.LoadSynchronous();
    UStaticMesh* EnvironmentMesh = LoadObject<UStaticMesh>(nullptr,
        TEXT("/Engine/EditorMeshes/AssetViewer/Sphere_inversenormals.Sphere_inversenormals"));
    UMaterialInterface* EnvironmentBaseMaterial = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Engine/EditorMaterials/AssetViewer/M_SkyBox.M_SkyBox"));
    if (!FloorMesh || !FloorMaterial || !EnvironmentTexture || !EnvironmentMesh
        || !EnvironmentBaseMaterial)
    {
        OutErrCode = ErrorCodes::ERR_ASSET_LOAD_FAILED;
        OutErrMsg = TEXT("Could not load the engine preview-scene floor or environment assets");
        return;
    }

    FloorComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    EnvironmentComponent = NewObject<UStaticMeshComponent>(
        GetTransientPackage(), NAME_None, RF_Transient);
    EnvironmentMaterial = UMaterialInstanceDynamic::Create(
        EnvironmentBaseMaterial, GetTransientPackage());
    if (!FloorComponent || !EnvironmentComponent || !EnvironmentMaterial)
    {
        OutErrCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
        OutErrMsg = TEXT("Could not allocate the transient preview-scene rig components");
        return;
    }

    EnvironmentMaterial->SetTextureParameterValue(TEXT("SkyBox"), EnvironmentTexture);
    EnvironmentMaterial->SetScalarParameterValue(TEXT("CubemapRotation"),
        Defaults.LightingRigRotation / 360.0f);
    EnvironmentMaterial->SetScalarParameterValue(TEXT("Intensity"), Defaults.SkyLightIntensity);
    EnvironmentComponent->SetStaticMesh(EnvironmentMesh);
    EnvironmentComponent->SetMaterial(0, EnvironmentMaterial);
    EnvironmentComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    EnvironmentComponent->CastShadow = false;
    EnvironmentComponent->bCastDynamicShadow = false;
    Scene->AddComponent(EnvironmentComponent,
        FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(2000.0)));

    FloorComponent->SetStaticMesh(FloorMesh);
    FloorComponent->SetMaterial(0, FloorMaterial);
    FloorComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FloorComponent->CastShadow = false;
    const double SafeRadius = FMath::Max(SubjectBoundsRadius, 1.0);
    const double FloorScale = FMath::Max(4.0, SafeRadius / 50.0);
    Scene->AddComponent(FloorComponent,
        FTransform(FloorRotation,
            FVector(SubjectBoundsOrigin.X, SubjectBoundsOrigin.Y,
                SubjectBoundsOrigin.Z - SafeRadius),
            FVector(FloorScale, FloorScale, 1.0)));

    Scene->SetLightDirection(Defaults.DirectionalLightRotation);
    Scene->SetLightBrightness(Defaults.DirectionalLightIntensity);
    Scene->SetLightColor(Defaults.DirectionalLightColor.ToFColor(/*bSRGB=*/true));
    Scene->SetSkyCubemap(EnvironmentTexture);
    Scene->SetSkyBrightness(Defaults.SkyLightIntensity);
    bCaptureContentsUpdated = UpdatePreviewSceneCaptures(Scene, bCaptureContentsIncomplete);

    EnvironmentComponent->SetVisibility(Defaults.bShowEnvironment, true);
    FloorComponent->SetVisibility(Defaults.bShowFloor, true);
    SkyCubemapPath = EnvironmentTexture->GetPathName();

    FEngineShowFlags DefaultShowFlags(ESFIM_Game);
    bInitialized = true;
    PreviousRig = Measure(DefaultShowFlags);
    if (Pin.WantsRig())
    {
        ApplySceneRigValues(*Scene, Pin, FloorComponent, EnvironmentComponent,
            EnvironmentMaterial);
        bCaptureContentsUpdated = UpdatePreviewSceneCaptures(Scene, bCaptureContentsIncomplete);
        bApplied = true;
    }
}

FPreviewSceneRigReport FScopedPreviewSceneRig::Measure(
    const FEngineShowFlags& ShowFlags) const
{
    if (!Scene)
    {
        return FPreviewSceneRigReport();
    }
    FPreviewSceneRigReport Report = MeasureSceneRigValues(*Scene, ShowFlags);
    Report.bBackdropVisibilityMeasured = bInitialized;
    Report.ProfileName = TEXT("PinWrightTransientMesh");
    Report.ProfileIndex = INDEX_NONE;
    if (Report.SkyCubemapPath.IsEmpty())
    {
        Report.SkyCubemapPath = SkyCubemapPath;
    }
    Report.bShowFloor = FloorComponent && FloorComponent->GetVisibleFlag();
    Report.bShowEnvironment = EnvironmentComponent && EnvironmentComponent->GetVisibleFlag();
    return Report;
}

bool CanApplyRig(const FEditorViewportClient& ConstClient, const FPreviewSceneRigPin& Pin,
    FString& OutErrCode, FString& OutErrMsg)
{
    OutErrCode.Reset();
    OutErrMsg.Reset();
    if (!Pin.WantsRig())
    {
        return true;
    }

    FEditorViewportClient& Client = const_cast<FEditorViewportClient&>(ConstClient);
    if (!Client.GetPreviewScene())
    {
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        OutErrMsg = TEXT("previewScene was requested for a viewport that has no preview scene at "
                         "all -- a level viewport draws the level's own lighting, which this "
                         "parameter deliberately does not touch. Refused rather than ignored: a "
                         "rig that silently does nothing returns a frame under a lighting setup "
                         "the caller believes they chose. Use previewScene only on the asset and "
                         "animation preview verbs, or on a camera verb whose `subject` names an "
                         "asset kind.");
        return false;
    }
    if (Pin.WantsAdvancedScene() && !IsAdvancedPreviewViewport(Client))
    {
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        OutErrMsg = TEXT("previewScene.showFloor / previewScene.showEnvironment need an "
                         "FAdvancedPreviewScene, and this viewport's preview scene is not one "
                         "(or its widget type is not on the verified allow-list in "
                         "PreviewSceneRig.cpp). The floor and the sky sphere are components that "
                         "only the advanced scene creates. previewScene.key and previewScene.sky "
                         "work on any preview scene and are unaffected.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The shared profile array and the committed config file
// ---------------------------------------------------------------------------------------------

FString PreviewSceneConfigFilePath()
{
    // Reflection rather than USharedProfiles::StaticClass(): that class carries no *_API macro
    // (AssetViewerSettings.h:320) so its StaticClass() cannot be linked from another module, and
    // hardcoding "Config/DefaultEditor.ini" would be exactly the assumption this whole file
    // exists to stop making.
    if (UClass* SharedProfilesClass =
            FindObject<UClass>(nullptr, TEXT("/Script/AdvancedPreviewScene.SharedProfiles")))
    {
        if (const UObject* Cdo = SharedProfilesClass->GetDefaultObject())
        {
            return Cdo->GetDefaultConfigFilename();
        }
    }
    // Only reachable before the AdvancedPreviewScene module has registered its classes. Same
    // printf UObject::GetDefaultConfigFilename ends in (Obj.cpp:3987) with the ClassConfigName
    // that `UCLASS(config = Editor, defaultconfig)` gives USharedProfiles.
    return FConfigCacheIni::NormalizeConfigIniPath(
        FString::Printf(TEXT("%sDefault%s.ini"), *FPaths::SourceConfigDir(), TEXT("Editor")));
}

FString DigestFile(const FString& AbsolutePath)
{
    if (AbsolutePath.IsEmpty() || !FPaths::FileExists(AbsolutePath))
    {
        // Empty compares equal to a later empty digest, so "the file did not exist before or
        // after" reports as unchanged -- which is the truth, not a missing measurement.
        return FString();
    }
    const FMD5Hash Hash = FMD5Hash::HashFile(*AbsolutePath);
    if (!Hash.IsValid())
    {
        return FString();
    }
    return LexToString(Hash);
}

FSharedProfileSnapshot CaptureSharedProfiles()
{
    FSharedProfileSnapshot Snapshot;
    if (UAssetViewerSettings* Settings = UAssetViewerSettings::Get())
    {
        Snapshot.Profiles = Settings->Profiles;
        Snapshot.bCaptured = true;
    }
    Snapshot.ConfigDigest = DigestFile(PreviewSceneConfigFilePath());
    return Snapshot;
}

bool SharedProfilesMatch(const TArray<FPreviewSceneProfile>& A,
    const TArray<FPreviewSceneProfile>& B)
{
    if (A.Num() != B.Num())
    {
        return false;
    }
    UScriptStruct* Struct = RigProfileStruct();
    if (!Struct)
    {
        // Cannot compare, so cannot claim equality. Reporting "different" makes the restore run,
        // which is the safe direction: an unnecessary assignment of the snapshot back over itself
        // costs a copy and changes nothing.
        return false;
    }
    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        if (!Struct->CompareScriptStruct(&A[Index], &B[Index], PPF_None))
        {
            return false;
        }
    }
    return true;
}

bool RestoreSharedProfiles(const FSharedProfileSnapshot& Snapshot, bool& bOutRestoreWasNeeded)
{
    bOutRestoreWasNeeded = false;
    if (!Snapshot.bCaptured)
    {
        // Nothing was ever snapshotted, so there is nothing this call could have left moved.
        return true;
    }
    UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
    if (!Settings)
    {
        return false;
    }
    if (SharedProfilesMatch(Settings->Profiles, Snapshot.Profiles))
    {
        return true;
    }

    Settings->Profiles = Snapshot.Profiles;
    bOutRestoreWasNeeded = true;

    // Third, and only because the array really moved. The broadcast drives a four-way UpdateScene
    // on every live FAdvancedPreviewScene in the editor (AdvancedPreviewScene.cpp:609, :630), so
    // an unconditional one would cost that on every capture in the session for nothing.
    Settings->OnAssetViewerSettingsChanged().Broadcast(NAME_None);

    if (!SharedProfilesMatch(Settings->Profiles, Snapshot.Profiles))
    {
        // The BROADCAST moved it again. That is not hypothetical: UpdateScene writes a scene's
        // component light rotation back into the shared profile whenever the two differ
        // (AdvancedPreviewScene.cpp:177-188), and the comparison is an exact FRotator::operator!=
        // against a rotator DERIVED FROM A VECTOR, so any other open preview scene in the editor
        // can trip it. Put the snapshot back once more WITHOUT broadcasting -- a second broadcast
        // would only invite a third -- and let the return value say whether it stuck. The shared
        // array is what the committed config file is written from, so it is the one that has to
        // end up right.
        Settings->Profiles = Snapshot.Profiles;
        return SharedProfilesMatch(Settings->Profiles, Snapshot.Profiles);
    }
    return true;
}

FScopedSharedProfiles::FScopedSharedProfiles()
    : Snapshot(CaptureSharedProfiles())
{
}

FScopedSharedProfiles::~FScopedSharedProfiles()
{
    bRestored = RestoreSharedProfiles(Snapshot, bRestoreWasNeeded);
}

// ---------------------------------------------------------------------------------------------
// The scoped rig
// ---------------------------------------------------------------------------------------------

FScopedPreviewSceneRig::FScopedPreviewSceneRig(FEditorViewportClient& InClient,
    const FPreviewSceneRigPin& Pin)
    : Client(&InClient)
    , Scene(InClient.GetPreviewScene())
{
    // ---- 1. Snapshot, BEFORE the WantsRig() early return ----
    // Invariant 1 of FScopedExposurePin: `previous` has to be reported on the omitted-parameter
    // path too, or a caller cannot tell "no rig was asked for" from "a rig was asked for and did
    // nothing" -- the pair decision 5 exists to keep distinguishable.
    PreviousRig = MeasureRig(*Client);
    if (UAssetViewerSettings* Settings = UAssetViewerSettings::Get())
    {
        ProfilesBefore = Settings->Profiles;
        bProfilesCaptured = true;
    }
    ConfigDigestBefore = DigestFile(PreviewSceneConfigFilePath());

    if (!Pin.WantsRig())
    {
        // The omitted-parameter path writes nothing to the scene and leaves bApplied false. The
        // PROFILE snapshot above is still taken and the destructor still restores it -- that is
        // deliberate and is the one place this guard departs from FScopedExposurePin's invariant
        // 3, because Defect 1 (SNiagaraSystemViewport::Construct writing bShowFloor=false into
        // the shared profile, SNiagaraSystemViewport.cpp:872) fires with no rig requested at all.
        // Restoring an array nothing changed is a comparison and no write, so the omitted path
        // still leaves the process exactly as it found it.
        //
        // The capture drain DOES run here, though, because the queue it empties has nothing to do
        // with this call's request: a capture that asked for no rig is lit by the same sky
        // capture the editor last left, and a 240-still set taken with no `previewScene` write at
        // all is the run that made that visible. See UpdatePreviewSceneCaptures.
        bCaptureContentsUpdated = UpdatePreviewSceneCaptures(Scene, bCaptureContentsIncomplete);
        return;
    }

    if (!Scene)
    {
        // Unreachable through the capture path, which calls CanApplyRig first and refuses. Left
        // as a guard rather than a check because a constructor cannot report.
        return;
    }

    // ---- 2. Floor / environment ----
    //
    // THE PROFILE FIELD IS WRITTEN AND THEN THE DIRECT SETTER IS CALLED, in that order, and NOT
    // the plain SetFloorVisibility(bVisible) the engine offers. Three reasons, all measured:
    //
    //  * SetFloorVisibility(bVisible) with bDirect defaulted false calls PostEditChangeProperty on
    //    the process-wide UAssetViewerSettings (AdvancedPreviewScene.cpp:391-403), which
    //    broadcasts to every live preview scene in the editor. That IS Defect 1.
    //  * SetFloorVisibility(true, /*bDirect=*/true) CANNOT force the floor visible: it ANDs the
    //    argument with the profile's own bShowFloor (:407). Only `false` is unconditional. So a
    //    showFloor:true request against a profile that hides the floor needs the profile field.
    //  * Writing the profile field directly is safe HERE and only here, because this guard
    //    snapshotted the whole array above and restores it below. Nothing calls
    //    UAssetViewerSettings::Save(), so nothing reaches the committed config file either way.
    if (FAdvancedPreviewScene* Advanced = RigAdvancedSceneFor(*Client))
    {
        if (FPreviewSceneProfile* Profile = CurrentProfile(*Advanced))
        {
            if (Pin.bShowFloorProvided)
            {
                Profile->bShowFloor = Pin.bShowFloor;
                Advanced->SetFloorVisibility(Pin.bShowFloor, /*bDirect=*/true);
            }
            if (Pin.bShowEnvironmentProvided)
            {
                Profile->bShowEnvironment = Pin.bShowEnvironment;
                Advanced->SetEnvironmentVisibility(Pin.bShowEnvironment, /*bDirect=*/true);
            }
        }
    }

    // ---- 3. Key and sky last ----
    // All four setters write ONLY the component (PreviewScene.cpp:275-324) and none of them
    // touches the profile. Last, because the floor/environment step above can call into
    // UpdateScene-adjacent engine code and the light direction is the value the write-back at
    // AdvancedPreviewScene.cpp:177-188 reads.
    ApplySceneRigValues(*Scene, Pin);

    // ---- 4. Drain the sky and reflection captures, AFTER every write above ----
    // Order matters both ways: before the writes it would recapture the rig this call is
    // replacing, and after Invalidate() it would leave the scheduled redraw reading the old
    // capture. Invalidate schedules a redraw and is not a tick, so it drives none of this itself.
    bCaptureContentsUpdated = UpdatePreviewSceneCaptures(Scene, bCaptureContentsIncomplete);

    Client->Invalidate();
    bApplied = true;
    bInitialized = true;
}

FScopedPreviewSceneRig::~FScopedPreviewSceneRig()
{
    if (bDisposable)
    {
        return;
    }
    // THE ORDER BELOW IS THE WHOLE DESIGN. Reversing steps 1 and 2 launders the override into the
    // shared profile: FAdvancedPreviewScene::UpdateScene reads GetLightDirection() and, when it
    // differs from Profile.DirectionalLightRotation, writes the COMPONENT's rotation into the
    // SHARED PROFILE (AdvancedPreviewScene.cpp:177-188) -- which the next "Preview Scene Settings"
    // tab teardown flushes to the committed Config/DefaultEditor.ini
    // (SAdvancedPreviewDetailsTab.cpp:46, UAssetViewerSettings::Save with no guard of any kind).
    // Restoring the component FIRST makes bLightDirChanged false, so that block never runs.

    // ---- 1. Components first ----
    if (bApplied)
    {
        if (Scene)
        {
            RestoreSceneRigValues(*Scene, PreviousRig);
        }
    }

    // ---- 2. Shared profile second, 3. broadcast third and only if step 2 moved something ----
    FSharedProfileSnapshot Snapshot;
    Snapshot.Profiles = ProfilesBefore;
    Snapshot.ConfigDigest = ConfigDigestBefore;
    Snapshot.bCaptured = bProfilesCaptured;
    bool bRestoreWasNeeded = false;
    bProfilesRestored = RestoreSharedProfiles(Snapshot, bRestoreWasNeeded);

    // Re-assert the floor and sky components from the RESTORED profile. Needed on the branch where
    // the profile array did not move (so no broadcast fired) but this guard had still written the
    // components through the bDirect setters. On the branch where the broadcast DID fire,
    // UpdateScene's tail already re-asserts both from the profile regardless of its own bUpdate*
    // flags (AdvancedPreviewScene.cpp:225-234), so this is a no-op there.
    if (bApplied)
    {
        if (FAdvancedPreviewScene* Advanced = RigAdvancedSceneFor(*Client))
        {
            if (const FPreviewSceneProfile* Profile = CurrentProfile(*Advanced))
            {
                Advanced->SetFloorVisibility(Profile->bShowFloor, /*bDirect=*/true);
                Advanced->SetEnvironmentVisibility(Profile->bShowEnvironment, /*bDirect=*/true);
            }
        }
    }

    // ---- 4. Measure ----
    ConfigDigestAfter = DigestFile(PreviewSceneConfigFilePath());

    if (bApplied)
    {
        Client->Invalidate();
    }
}
}
