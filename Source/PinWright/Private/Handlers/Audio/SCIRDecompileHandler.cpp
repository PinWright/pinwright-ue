// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/SoundCueDumpBuilder.h"

#include "SCIR/SCIRDecompiler.h"
#include "Utils/IrSidecarRegistry.h"

#include "Dom/JsonValue.h"
#include "Sound/SoundCue.h"

namespace
{
    UClass* GetSCIRSoundCueClass()
    {
        return USoundCue::StaticClass();
    }

    IrSidecarRegistry::FIrSidecarResult BuildSCIRSoundCueSidecar(UObject* Asset)
    {
        const FSCIRResult Result = SCIRDecompiler::BuildSoundCueIrText(Cast<USoundCue>(Asset));
        return {Result.Text, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("scir.sound_cue"), DumpFileNames::Scir,
    &GetSCIRSoundCueClass, &BuildSCIRSoundCueSidecar, 100);

REGISTER_RPC_HANDLER("audio.authoring.decompile_sound_cue", "audio.authoring",
    "Decompile a SoundCue graph into SCIR text.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundCue asset")
    ))
{
    FString AssetPathRaw;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPathRaw))
    {
        return true;
    }

    FString AssetPath;
    USoundCue* Cue = SoundCueDumpBuilder::LoadSoundCueFromPath(AssetPathRaw, &AssetPath);
    if (!Cue)
    {
        const FString DisplayPath = AssetPath.IsEmpty() ? AssetPathRaw : AssetPath;
        Ctx.SendError(TEXT("CUE_NOT_FOUND"), FString::Printf(TEXT("Could not load SoundCue: %s"), *DisplayPath));
        return true;
    }

    const FSCIRResult DecompileResult = SCIRDecompiler::BuildSoundCueIrText(Cue);
    if (!DecompileResult.bSuccess)
    {
        const FString Message = DecompileResult.Warnings.IsEmpty()
            ? TEXT("SCIR decompile failed.")
            : FString::Join(DecompileResult.Warnings, TEXT("\n"));
        Ctx.SendError(TEXT("SCIR_DECOMPILE_FAILED"), Message);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : DecompileResult.Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("ir"), DecompileResult.Text);
    Result->SetStringField(TEXT("text"), DecompileResult.Text);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Ctx.SendSuccess(Result);
    return true;
}
