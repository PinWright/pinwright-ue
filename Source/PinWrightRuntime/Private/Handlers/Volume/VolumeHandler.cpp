// Copyright (c) 2026 Alexander Penkin. MIT License.

// VolumeHandler.cpp (runtime half) - volume.get_volumes_info, the read-only volume verb.
// Volume creation and extent/property edits build BSP brushes in the editor world and stay
// in the editor module's Handlers/Volume/VolumeHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Dispatch/PinWrightWorld.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/TriggerBase.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Volume.h"

namespace VolumeInfoHelpers
{
    TSharedPtr<FJsonObject> MakeVolumeInfo(AActor* Actor)
    {
        TSharedPtr<FJsonObject> VolumeInfo = MakeShareable(new FJsonObject());
        VolumeInfo->SetStringField(TEXT("name"), ActorNameParamUtils::ActorLabelOrName(Actor));
        VolumeInfo->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

        const FVector Location = Actor->GetActorLocation();
        TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject());
        LocationJson->SetNumberField(TEXT("x"), Location.X);
        LocationJson->SetNumberField(TEXT("y"), Location.Y);
        LocationJson->SetNumberField(TEXT("z"), Location.Z);
        VolumeInfo->SetObjectField(TEXT("location"), LocationJson);

        FVector Origin, BoxExtent;
        Actor->GetActorBounds(false, Origin, BoxExtent);
        TSharedPtr<FJsonObject> ExtentJson = MakeShareable(new FJsonObject());
        ExtentJson->SetNumberField(TEXT("x"), BoxExtent.X);
        ExtentJson->SetNumberField(TEXT("y"), BoxExtent.Y);
        ExtentJson->SetNumberField(TEXT("z"), BoxExtent.Z);
        VolumeInfo->SetObjectField(TEXT("extent"), ExtentJson);
        return VolumeInfo;
    }
}

REGISTER_RPC_HANDLER("volume.get_volumes_info", "volume", "Get information about all volumes in the level",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Name filter"),
        RPC_PARAM_OPT("volumeType", "string", "Volume type filter")
    ))
{
    using namespace VolumeInfoHelpers;

    FString Filter = Ctx.GetString(TEXT("filter"), TEXT(""));
    FString VolumeType = Ctx.GetString(TEXT("volumeType"), TEXT(""));

    // Editor: the editor world (never PIE), as before the split. Game: the live world.
    UWorld* World = PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
    if (!World) { Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("Editor world not available")); return true; }

    TArray<TSharedPtr<FJsonValue>> VolumesArray;
    int32 TotalCount = 0;

    for (TActorIterator<AVolume> It(World); It; ++It)
    {
        AVolume* Volume = *It;
        if (!Volume) continue;

        if (!VolumeType.IsEmpty())
        {
            FString ClassName = Volume->GetClass()->GetName();
            if (!ClassName.Contains(VolumeType)) continue;
        }
        if (!Filter.IsEmpty())
        {
            if (!ActorNameParamUtils::ActorLabelOrName(Volume).Contains(Filter)) continue;
        }

        VolumesArray.Add(MakeShareable(new FJsonValueObject(MakeVolumeInfo(Volume))));
        TotalCount++;
    }

    for (TActorIterator<ATriggerBase> It(World); It; ++It)
    {
        ATriggerBase* Trigger = *It;
        if (!Trigger) continue;

        if (!VolumeType.IsEmpty())
        {
            FString ClassName = Trigger->GetClass()->GetName();
            if (!ClassName.Contains(VolumeType) && !VolumeType.Equals(TEXT("Trigger"), ESearchCase::IgnoreCase)) continue;
        }
        if (!Filter.IsEmpty())
        {
            if (!ActorNameParamUtils::ActorLabelOrName(Trigger).Contains(Filter)) continue;
        }

        VolumesArray.Add(MakeShareable(new FJsonValueObject(MakeVolumeInfo(Trigger))));
        TotalCount++;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    TSharedPtr<FJsonObject> VolumesInfo = MakeShareable(new FJsonObject());
    VolumesInfo->SetNumberField(TEXT("totalCount"), TotalCount);
    VolumesInfo->SetArrayField(TEXT("volumes"), VolumesArray);
    Result->SetObjectField(TEXT("volumesInfo"), VolumesInfo);

    Ctx.SendSuccess(Result);
    return true;
}
