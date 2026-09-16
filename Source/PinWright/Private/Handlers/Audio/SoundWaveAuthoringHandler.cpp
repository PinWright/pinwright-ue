// Copyright (c) 2026 Alexander Penkin. MIT License.

// SoundWaveAuthoringHandler.cpp - audio.authoring.set_sound_wave_properties
//
// Sibling of AudioAuthoringHandler.cpp. Kept in a separate file to avoid
// touching that file's in-flight diff. Mirrors the per-field conditional
// pattern of audio.authoring.set_class_properties / set_cue_properties:
// each field is applied only when present in the JSON payload, otherwise
// the live USoundWave value is preserved.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "Utils/PropertyChangeNotify.h"

#include "Sound/SoundWave.h"
#include "Sound/SoundGroups.h"
#include "Dom/JsonObject.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    // Inlined ~6-line equivalent of LoadSoundWaveFromPath in AudioAuthoringHandler.cpp:199.
    // Kept local to avoid taking a dependency on a private static in the sibling file.
    USoundWave* LoadSoundWaveByPath(const FString& InPath)
    {
        FString Normalized = InPath;
        Normalized.ReplaceInline(TEXT("\\Content\\"), TEXT("/Game/"));
        Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Normalized.EndsWith(TEXT("/")))
        {
            Normalized.LeftChopInline(1);
        }
        return Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *Normalized));
    }
}

REGISTER_RPC_HANDLER("audio.authoring.set_sound_wave_properties", "audio.authoring",
    "Set per-wave properties on a USoundWave (bLooping, volume, pitch, soundGroup, compressionQuality, bMature, bSingleLine).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the USoundWave"),
        RPC_PARAM_OPT("bLooping", "boolean", "Whether the wave loops indefinitely when played directly"),
        RPC_PARAM_OPT("volume", "number", "Per-wave volume multiplier"),
        RPC_PARAM_OPT("pitch", "number", "Per-wave pitch multiplier"),
        RPC_PARAM_OPT("soundGroup", "string", "ESoundGroup enum name (e.g. SOUNDGROUP_Default, SOUNDGROUP_Voice, SOUNDGROUP_Effects)"),
        RPC_PARAM_OPT("compressionQuality", "number", "int32 compression quality, 0..100"),
        RPC_PARAM_OPT("bMature", "boolean", "Mature content flag"),
        RPC_PARAM_OPT("bSingleLine", "boolean", "Single-line UI flag for editor display"),
        RPC_PARAM_DEF("save", "boolean", "Mark dirty + notify asset registry after edits", "true")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    USoundWave* Wave = LoadSoundWaveByPath(AssetPath);
    if (!Wave)
    {
        Ctx.SendError(TEXT("SOUND_WAVE_NOT_FOUND"),
            FString::Printf(TEXT("Could not load USoundWave: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> P = Ctx.GetRawPayload();
    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    UPackage* const WavePackage = Wave->GetOutermost();
    const bool bPackageWasDirty = WavePackage && WavePackage->IsDirty();
    const bool bCompressionQualityChanged = P->HasField(TEXT("compressionQuality"));
    const bool bHasPropertyEdits =
        P->HasField(TEXT("bLooping"))
        || P->HasField(TEXT("volume"))
        || P->HasField(TEXT("pitch"))
        || P->HasField(TEXT("soundGroup"))
        || bCompressionQualityChanged
        || P->HasField(TEXT("bMature"))
        || P->HasField(TEXT("bSingleLine"));

    if (P->HasField(TEXT("bLooping")))
    {
        Wave->bLooping = Ctx.GetBool(TEXT("bLooping"), false) ? 1 : 0;
    }
    if (P->HasField(TEXT("volume")))
    {
        Wave->Volume = static_cast<float>(Ctx.GetNumber(TEXT("volume"), 1.0));
    }
    if (P->HasField(TEXT("pitch")))
    {
        Wave->Pitch = static_cast<float>(Ctx.GetNumber(TEXT("pitch"), 1.0));
    }
    if (P->HasField(TEXT("compressionQuality")))
    {
        // UE 5.6 made USoundWave::CompressionQuality private with only a getter exposed.
        // Set via UPROPERTY reflection to preserve the field's editor-visible semantics
        // (Config/EditAnywhere/AssetRegistrySearchable) without friending or const_cast.
        const int32 NewQuality = Ctx.GetInt(TEXT("compressionQuality"), 40);
        if (FIntProperty* Prop = FindFProperty<FIntProperty>(USoundWave::StaticClass(), TEXT("CompressionQuality")))
        {
            Prop->SetPropertyValue_InContainer(Wave, NewQuality);
        }
    }
    if (P->HasField(TEXT("bMature")))
    {
        Wave->bMature = Ctx.GetBool(TEXT("bMature"), false) ? 1 : 0;
    }
    if (P->HasField(TEXT("bSingleLine")))
    {
        Wave->bSingleLine = Ctx.GetBool(TEXT("bSingleLine"), false) ? 1 : 0;
    }
    if (P->HasField(TEXT("soundGroup")))
    {
        const FString GroupName = Ctx.GetString(TEXT("soundGroup"));
        const UEnum* GroupEnum = StaticEnum<ESoundGroup>();
        const int64 Val = GroupEnum ? GroupEnum->GetValueByNameString(GroupName) : INDEX_NONE;
        if (Val == INDEX_NONE)
        {
            Ctx.SendError(TEXT("INVALID_SOUND_GROUP"),
                FString::Printf(TEXT("Unknown ESoundGroup name: %s"), *GroupName));
            return true;
        }
        Wave->SoundGroup = static_cast<ESoundGroup>(Val);
    }

    if (bHasPropertyEdits)
    {
        // Raw UPROPERTY writes do not update FSoundWaveData or the published proxy.
        // Notify the actual changed property so USoundWave can run its editor refresh
        // path. CompressionQuality takes the engine's UpdateAsset branch (which also
        // invalidates compressed data); ordinary fields initialize the working metadata,
        // then UpdatePlatformData publishes it to the runtime proxy.
        const TCHAR* RefreshPropertyName = TEXT("bLooping");
        if (bCompressionQualityChanged)
        {
            RefreshPropertyName = TEXT("CompressionQuality");
        }
        else if (P->HasField(TEXT("volume")))
        {
            RefreshPropertyName = TEXT("Volume");
        }
        else if (P->HasField(TEXT("pitch")))
        {
            RefreshPropertyName = TEXT("Pitch");
        }
        else if (P->HasField(TEXT("soundGroup")))
        {
            RefreshPropertyName = TEXT("SoundGroup");
        }
        else if (P->HasField(TEXT("bMature")))
        {
            RefreshPropertyName = TEXT("bMature");
        }
        else if (P->HasField(TEXT("bSingleLine")))
        {
            RefreshPropertyName = TEXT("bSingleLine");
        }

        FProperty* RefreshProperty = FindFProperty<FProperty>(
            USoundWave::StaticClass(), RefreshPropertyName);
        if (PinWright::NotifyPropertyChanged(Wave, RefreshProperty))
        {
            if (!bCompressionQualityChanged)
            {
                Wave->UpdatePlatformData();
            }
            if (!bSave && WavePackage && !bPackageWasDirty)
            {
                WavePackage->SetDirtyFlag(false);
            }
        }
    }

    if (bSave)
    {
        McpSafeAssetSave(Wave);
    }

    // Echo post-edit field values so callers can verify the round-trip without a
    // separate describe_sound_wave call. Field set matches SoundWaveDumpBuilder.
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("bLooping"), Wave->bLooping != 0);
    Result->SetNumberField(TEXT("volume"), Wave->Volume);
    Result->SetNumberField(TEXT("pitch"), Wave->Pitch);
    Result->SetNumberField(TEXT("compressionQuality"), Wave->GetCompressionQuality());
    Result->SetBoolField(TEXT("bMature"), Wave->bMature != 0);
    Result->SetBoolField(TEXT("bSingleLine"), Wave->bSingleLine != 0);
    if (const UEnum* GroupEnum = StaticEnum<ESoundGroup>())
    {
        Result->SetStringField(TEXT("soundGroup"),
            GroupEnum->GetNameStringByValue(static_cast<int64>(Wave->SoundGroup)));
    }
    Result->SetStringField(TEXT("message"), TEXT("SoundWave properties updated"));
    AddAssetVerification(Result, Wave);
    Ctx.SendSuccess(Result);
    return true;
}
