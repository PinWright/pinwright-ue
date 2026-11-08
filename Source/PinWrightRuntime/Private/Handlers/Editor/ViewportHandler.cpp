// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.screenshot: the game-safe verb of the editor module's ViewportHandler.cpp, which keeps the
// viewport-camera verbs and installs the level-editor fallback (ScreenshotLevelFallback.h).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/ScreenshotLevelFallback.h"
#include "Handlers/Render/CaptureExposure.h"
#include "Utils/CaptureReadinessGate.h"
#include "Utils/OnScreenMessageSurvey.h"
#include "Utils/RenderingAvailability.h"
#include "Utils/ScreenshotUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"

namespace PinWrightScreenshotLevelFallback
{
    // Constant-initialised, so the editor module's static-init Set() never races a dynamic init here.
    static FCaptureFn GScreenshotLevelFallbackCapture = nullptr;

    void Set(FCaptureFn Fn)
    {
        GScreenshotLevelFallbackCapture = Fn;
    }

    FCaptureFn Get()
    {
        return GScreenshotLevelFallbackCapture;
    }
}

// ---- editor.screenshot ----
REGISTER_RPC_HANDLER("editor.screenshot", "editor", "Capture a PNG screenshot into Saved/Screenshots/. Uses the game/PIE viewport when one exists; otherwise falls back to the active level-editor viewport (editor only; a -game process without a game viewport answers NO_VIEWPORT). Optional paired width/height render an exact-size frame; on the game viewport this includes Slate/UMG at the requested UIScaleCurve value. Runs as a synchronously-completed tracked job: the response is sent after the PNG is written and carries the result plus ticket_id. Filename is sanitised against path traversal.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/. Auto-generated as 'Screenshot_<timestamp>.png' when empty. The .png extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Exact output width in pixels. Must be supplied together with height; omit both for the live viewport size."),
        RPC_PARAM_OPT("height", "number", "Exact output height in pixels. Must be supplied together with width; omit both for the live viewport size."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("location", "object", "Camera location {x, y, z} for this shot only. On the game/PIE viewport it replaces the local player's view point for the capture draw (nothing persistent moves); on the Level Editor fallback it is the capture camera, restored afterwards. Omitted: the current camera's location."),
        RPC_PARAM_OPT("rotation", "object", "Camera rotation {pitch, yaw, roll} for this shot only, with the same scope as location. Omitted: the current camera's rotation.")
    ))
{
  if (!PinWrightRendering::RequireRenderer(Ctx))
  {
    return true;
  }

  const FString RequestedFilename = Ctx.GetString(TEXT("filename"));
  const TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
  const bool bHasWidth = Payload.IsValid() && Payload->HasField(TEXT("width"));
  const bool bHasHeight = Payload.IsValid() && Payload->HasField(TEXT("height"));
  if (bHasWidth != bHasHeight)
  {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          TEXT("width and height must be supplied together, or both omitted"));
      return true;
  }

  FIntPoint RequestedSize = FIntPoint::ZeroValue;
  if (bHasWidth)
  {
      RequestedSize.X = Ctx.GetInt(TEXT("width"));
      RequestedSize.Y = Ctx.GetInt(TEXT("height"));
      const int64 RequestedPixels = static_cast<int64>(RequestedSize.X)
          * static_cast<int64>(RequestedSize.Y);
      if (RequestedSize.X <= 0 || RequestedSize.Y <= 0
          || RequestedSize.X > PinWrightScreenshotUtils::MaxGameViewportCaptureDimension
          || RequestedSize.Y > PinWrightScreenshotUtils::MaxGameViewportCaptureDimension
          || RequestedPixels > PinWrightScreenshotUtils::MaxGameViewportCapturePixels)
      {
          Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
              FString::Printf(TEXT("width and height must be in (0, %d] and total at most %lld pixels"),
                  PinWrightScreenshotUtils::MaxGameViewportCaptureDimension,
                  static_cast<long long>(PinWrightScreenshotUtils::MaxGameViewportCapturePixels)));
          return true;
      }
  }

  PinWrightRenderCapture::FExposurePin Exposure;
  FString ParseErrorCode;
  FString ParseErrorMessage;
  if (!PinWrightRenderCapture::ParseExposurePin(
          Payload, Exposure, ParseErrorCode, ParseErrorMessage))
  {
      Ctx.SendError(ParseErrorCode, ParseErrorMessage);
      return true;
  }

  TOptional<FVector> AimLocation;
  TOptional<FRotator> AimRotation;
  if (Payload.IsValid()
      && ((Payload->HasField(TEXT("location")) && !Payload->HasTypedField<EJson::Object>(TEXT("location")))
          || (Payload->HasField(TEXT("rotation")) && !Payload->HasTypedField<EJson::Object>(TEXT("rotation")))))
  {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          TEXT("location must be an object {x, y, z} and rotation an object {pitch, yaw, roll}"));
      return true;
  }
  if (Payload.IsValid() && Payload->HasField(TEXT("location")))
  {
      AimLocation = Ctx.GetVector(TEXT("location"));
  }
  if (Payload.IsValid() && Payload->HasField(TEXT("rotation")))
  {
      AimRotation = Ctx.GetRotator(TEXT("rotation"));
  }
  if ((AimLocation.IsSet() && AimLocation->ContainsNaN())
      || (AimRotation.IsSet() && AimRotation->ContainsNaN()))
  {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("location and rotation must be finite"));
      return true;
  }

  FString Filename;
  const FString OutPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
      RequestedFilename, TEXT("Screenshot"), FString(), Filename);

  FJobBindArgs Args;
  Args.Method = TEXT("editor.screenshot");
  Args.StartedPayload = MakeShared<FJsonObject>();
  Args.StartedPayload->SetStringField(TEXT("requested_path"), OutPath);
  // The capture below runs inline and writes the PNG before OnComplete. Reply after it, so a
  // success response means the file is already on disk (B-editor-screenshot-returns-before-png-exists).
  Args.bCompletesInBind = true;

  Args.BindNativeDelegate =
      [OutPath, RequestedFilename, RequestedSize, Exposure, AimLocation, AimRotation](FJobOnComplete OnComplete)
  {
      // The readback preamble's shared state. Both branches fill it immediately before their own
      // readback -- never at handler entry, where the pose that queues the compiles has not been
      // applied yet.
      const TSharedRef<PinWrightCaptureReadiness::FReadinessResult> Readiness =
          MakeShared<PinWrightCaptureReadiness::FReadinessResult>();
      const auto RefuseNotReady = [&OnComplete, Readiness](const FString& Message)
      {
          // The job registry keeps the payload on a failed ticket, so the evidence and the reason
          // stay readable through system.job_status while the error code stays greppable.
          TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
          PinWrightCaptureReadiness::AddReadinessFields(*Readiness, Details);
          Details->SetStringField(TEXT("message"), Message);
          OnComplete(false, Details, ErrorCodes::ERR_CAPTURE_NOT_READY);
      };

      if (!GEngine || !GEngine->GameViewport)
      {
          // No game/PIE viewport: the editor module captures the active level-editor viewport
          // (Handlers/Editor/ScreenshotLevelFallback.h); a game process has none to fall back to.
          if (const PinWrightScreenshotLevelFallback::FCaptureFn Fallback = PinWrightScreenshotLevelFallback::Get())
          {
              Fallback(RequestedFilename, RequestedSize, Exposure, AimLocation, AimRotation, OnComplete);
          }
          else
          {
              OnComplete(false, nullptr, ErrorCodes::ERR_NO_VIEWPORT);
          }
          return;
      }
      // Game/PIE viewport active. The async FScreenshotRequest +
      // OnScreenshotCaptured delegate does not reliably fire under PIE-in-editor,
      // which left this job hung in "running" forever. Capture synchronously via
      // the shared game-viewport->PNG path (the same mechanism ui.screenshot uses
      // on this exact viewport) and complete the job in-line — no async wait, no
      // watchdog.
      int32 Width = 0, Height = 0;
      FString ErrorCode;
      PinWrightScreenshotUtils::FGameViewportCaptureOptions CaptureOptions;
      CaptureOptions.OutputSize = RequestedSize;
      CaptureOptions.bPinExposure = Exposure.WantsPin();
      CaptureOptions.ExposureEv100 = Exposure.Ev100;
      CaptureOptions.bObserveExposure =
          Exposure.Mode == PinWrightRenderCapture::EExposureRequestMode::Auto;
      CaptureOptions.AimLocation = AimLocation;
      CaptureOptions.AimRotation = AimRotation;

      // Before the readback, but sampled at the LIVE pose: an aimed capture (location/rotation)
      // moves the view inside CaptureGameViewportToPngFile, after this drain, so materials first
      // seen from the aimed pose are not gated here and the aimed frame gets no warm-up. The flush
      // half runs inside that call, at the last point before its three readback branches, and
      // comes back on the metadata.
      *Readiness = PinWrightCaptureReadiness::DrainBeforeReadback();
      if (Readiness->ShouldRefuseReadback())
      {
          RefuseNotReady(PinWrightCaptureReadiness::MakeRefusalMessage(*Readiness));
          return;
      }

      PinWrightScreenshotUtils::FGameViewportCaptureMetadata Metadata;
      if (PinWrightScreenshotUtils::CaptureGameViewportToPngFile(
              OutPath, Width, Height, ErrorCode, nullptr, &CaptureOptions, &Metadata))
      {
          auto R = MakeShared<FJsonObject>();
          R->SetNumberField(TEXT("width"),  Width);
          R->SetNumberField(TEXT("height"), Height);
          R->SetStringField(TEXT("path"),   OutPath);
          R->SetStringField(TEXT("captureSource"), TEXT("gameViewport"));
          R->SetBoolField(TEXT("fixedSize"), RequestedSize != FIntPoint::ZeroValue);
          R->SetStringField(TEXT("captureMode"), Metadata.bUsedOffscreenComposite
              ? TEXT("fixedSizeScenePlusUmg")
              : (Metadata.bUsedNativeBackBuffer
                  ? TEXT("nativeBackBuffer")
                  : TEXT("sceneOnlyFallback")));
          R->SetNumberField(TEXT("dpiScale"), Metadata.DpiScale);
          if (Metadata.bUsedOffscreenComposite)
          {
              // Measured per browser in the off-screen pass, so an image missing the web UI says so.
              TArray<TSharedPtr<FJsonValue>> Omitted;
              for (const FString& Type : Metadata.OmittedWidgets)
              {
                  Omitted.Add(MakeShared<FJsonValueString>(Type));
              }
              R->SetArrayField(TEXT("omittedWidgets"), Omitted);
              if (Omitted.Num() > 0)
              {
                  R->SetStringField(TEXT("warning"), FString::Printf(
                      TEXT("%d visible web browser view(s) (CEF) contributed no pixels to this "
                           "fixed-size composite, so web UI on screen is missing from the image. "
                           "Capture without width/height (captureMode nativeBackBuffer) to include it."),
                      Omitted.Num()));
              }
          }
          R->SetBoolField(TEXT("viewportRestored"), Metadata.bViewportRestored);

          TSharedPtr<FJsonObject> ExposureInfo = MakeShared<FJsonObject>();
          ExposureInfo->SetStringField(TEXT("mode"),
              PinWrightRenderCapture::ExposureModeKey(Exposure.Mode));
          ExposureInfo->SetBoolField(TEXT("pinRequested"), Exposure.WantsPin());
          ExposureInfo->SetBoolField(TEXT("pinned"), Metadata.bExposureApplied);
          ExposureInfo->SetBoolField(TEXT("restored"), Metadata.bExposureRestored);
          ExposureInfo->SetNumberField(TEXT("viewCount"), Metadata.ExposureViewCount);
          if (Metadata.ExposureViewCount > 0)
          {
              // Read off the drawn view (the family override the renderer received and the view
              // state's eye-adaptation readback), never echoed from the request. Same field
              // contract as the level path's viewport.exposure block.
              ExposureInfo->SetBoolField(TEXT("fixed"), Metadata.bFamilyExposureFixed);
              float AdaptedGain = 0.0f;
              const TCHAR* AdaptedSource = TEXT("readback");
              if (Metadata.bFamilyExposureFixed)
              {
                  ExposureInfo->SetNumberField(TEXT("ev100"), Metadata.FamilyFixedEv100);
                  AdaptedGain = PinWrightRenderCapture::Ev100ToExposureGain(Metadata.FamilyFixedEv100);
                  AdaptedSource = TEXT("fixedPin");
              }
              else
              {
                  AdaptedGain = Metadata.ReadbackExposureGain;
              }
              const bool bAdaptedMeasured = AdaptedGain > 0.0f;
              ExposureInfo->SetBoolField(TEXT("adaptedMeasured"), bAdaptedMeasured);
              if (bAdaptedMeasured)
              {
                  ExposureInfo->SetNumberField(TEXT("adapted"), AdaptedGain);
                  ExposureInfo->SetNumberField(TEXT("ev100Equivalent"),
                      PinWrightRenderCapture::ExposureGainToEv100(AdaptedGain));
                  ExposureInfo->SetStringField(TEXT("adaptedSource"), AdaptedSource);
              }
              else
              {
                  ExposureInfo->SetStringField(TEXT("adaptedReadbackPending"),
                      TEXT("The game viewport's view state has no completed eye-adaptation "
                           "readback yet, so no measured `adapted` / `ev100Equivalent` is "
                           "available. Let PIE draw a few frames and capture again."));
              }
          }
          if (Exposure.WantsPin())
          {
              ExposureInfo->SetNumberField(TEXT("ev100Requested"), Exposure.Ev100);
          }
          R->SetObjectField(TEXT("exposure"), ExposureInfo);
          if (Metadata.bAimRequested)
          {
              // Measured on the view the frame was drawn from; a miss already failed the capture
              // with CAPTURE_CAMERA_NOT_APPLIED.
              TSharedPtr<FJsonObject> Aim = MakeShared<FJsonObject>();
              Aim->SetBoolField(TEXT("applied"), Metadata.bAimApplied);
              Aim->SetObjectField(TEXT("location"),
                  PinWrightRenderCapture::MakeVectorObject(Metadata.ViewLocation));
              Aim->SetObjectField(TEXT("rotation"),
                  PinWrightRenderCapture::MakeRotatorObject(Metadata.ViewRotation));
              R->SetObjectField(TEXT("aim"), Aim);
          }

          // The game/PIE path publishes no FViewportCaptureOutput, so it carries the two
          // frame-state blocks on a `viewport` object of its own: what was still compiling when
          // this readback went out, and what text the engine drew into the pixels.
          Readiness->bReadbackFlushed = Metadata.bReadbackFlushed;
          const TSharedPtr<FJsonObject> ViewportBlock = MakeShared<FJsonObject>();
          PinWrightCaptureReadiness::AddReadinessFields(*Readiness, ViewportBlock);
          PinWrightOnScreenMessages::AddOnScreenMessageFields(
              PinWrightOnScreenMessages::Survey(), ViewportBlock);
          R->SetObjectField(TEXT("viewport"), ViewportBlock);
          OnComplete(true, R, FString());
      }
      else
      {
          OnComplete(false, nullptr, ErrorCode);
      }
  };
  Ctx.StartJob(Args);
  return true;
}
