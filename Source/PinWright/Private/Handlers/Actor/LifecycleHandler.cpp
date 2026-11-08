// Copyright (c) 2026 Alexander Penkin. MIT License.

// LifecycleHandler.cpp (editor module) - actor.export, editor-only: the T3D actor exporter
// lives in UnrealEd. actor.delete / actor.duplicate / actor.delete_by_tag are in the runtime
// module (Source/PinWrightRuntime/Private/Handlers/Actor/LifecycleHandler.cpp).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Utils/LogUtils.h"
#include "Dom/JsonObject.h"

#include "GameFramework/Actor.h"
#include "Exporters/Exporter.h"
#include "Misc/OutputDevice.h"

// ---- actor.export ----
REGISTER_RPC_HANDLER("actor.export", "actor", "Serialize an actor and its components to UE's T3D (text) format and return the text in the response. Useful for capturing actor state for later re-import or for diffing scene changes.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor to export.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  FMcpOutputCapture OutputCapture;
  UExporter::ExportToOutputDevice(nullptr, Found, nullptr, OutputCapture,
                                  TEXT("T3D"), 0, 0, false);
  FString OutputString = FString::Join(OutputCapture.Consume(), TEXT("\n"));

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("t3d"), OutputString);
  Data->SetStringField(TEXT("actorName"), ActorNameParamUtils::ActorLabelOrName(Found));
  Ctx.SendSuccess(Data);
  return true;
}
