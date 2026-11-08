// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Compat/JsonKeyCompat.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveGameInput.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveOsGesture.h"
#include "Handlers/Drive/DriveOsInput.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

// drive.os_gesture: a real-pointer (XTEST) gesture at a PIE game-viewport pixel or a world actor,
// with button, hold time and motion while pressed. drive.click / drive.drag aim at UMG handles and
// inject from one blocking call, so the engine sees a whole click in one frame; this verb paces the
// gesture across engine ticks (FDriveOsGesture::Start) so game code that polls the mouse per frame
// (a gizmo drag, a box select) sees the button held. It answers once the release has been pumped,
// with what was done; it does not settle on UI, so read the game state back separately.

namespace DriveOsGestureHandlerLocal
{
    // A gesture point as the caller named it: a viewport pixel, or an actor (and component).
    struct FPointSpec
    {
        bool bActor = false;
        FString Actor;
        FString Component;
        FVector2D Pixel = FVector2D::ZeroVector;
    };

    // Parse {x, y} or {actor, component?}. Sends INVALID_ARGUMENT naming Key and returns false otherwise.
    bool ParsePointSpec(const FHandlerContext& Ctx, const TCHAR* Key, const TSharedPtr<FJsonObject>& Obj, FPointSpec& Out)
    {
        static const TSet<FString> Allowed = { TEXT("x"), TEXT("y"), TEXT("actor"), TEXT("component") };
        TArray<FString> Keys;
        if (Obj.IsValid())
        {
            for (const auto& Field : Obj->Values)
            {
                Keys.Add(EARGCompat::JsonKeyToString(Field.Key));
            }
        }
        const bool bKnownKeys = Obj.IsValid() && Keys.FilterByPredicate([](const FString& K) { return !Allowed.Contains(K); }).Num() == 0;
        double X = 0.0;
        double Y = 0.0;
        const bool bHasActor = bKnownKeys && Obj->TryGetStringField(TEXT("actor"), Out.Actor) && !Out.Actor.IsEmpty();
        const bool bHasX = bKnownKeys && Obj->TryGetNumberField(TEXT("x"), X);
        const bool bHasY = bKnownKeys && Obj->TryGetNumberField(TEXT("y"), Y);
        const bool bHasComponent = bKnownKeys && Obj->HasField(TEXT("component"));
        if (bHasActor && !bHasX && !bHasY)
        {
            Out.bActor = true;
            if (bHasComponent && !Obj->TryGetStringField(TEXT("component"), Out.Component))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("%s.component must be a string (a component name on the actor)."), Key));
                return false;
            }
            return true;
        }
        if (!bHasActor && !bHasComponent && bHasX && bHasY)
        {
            Out.Pixel = FVector2D(X, Y);
            return true;
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("%s must be either {x, y} (PIE game-viewport pixels, origin top-left) or {actor, component?} (an actor in the PIE world, by label, name or path); got keys [%s]."),
                Key, *FString::Join(Keys, TEXT(", "))));
        return false;
    }

    // The world point a gesture aims at on an actor: the named component's bounds center, else
    // the actor's bounds center, else its location.
    bool ActorAimPoint(const FHandlerContext& Ctx, AActor* Actor, const FString& ComponentName, FVector& OutPoint)
    {
        if (ComponentName.IsEmpty())
        {
            const FBox Box = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
            OutPoint = Box.IsValid ? Box.GetCenter() : Actor->GetActorLocation();
            return true;
        }
        TArray<USceneComponent*> Components;
        Actor->GetComponents(Components);
        for (USceneComponent* Component : Components)
        {
            if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
            {
                const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
                OutPoint = Primitive ? FVector(Primitive->Bounds.Origin) : Component->GetComponentLocation();
                return true;
            }
        }
        Ctx.SendError(ErrorCodes::ERR_COMPONENT_NOT_FOUND,
            FString::Printf(TEXT("Actor '%s' has no scene component named '%s'."), *Actor->GetName(), *ComponentName));
        return false;
    }

    TSharedPtr<FJsonObject> PointJson(double X, double Y)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        return Obj;
    }

    // Resolve a spec to a game-viewport pixel. An actor is projected through the player's view
    // (OUT_OF_BOUNDS behind the camera); OutTarget describes it. Sends the error and returns false.
    bool ResolvePixel(const FHandlerContext& Ctx, const FDriveGameInputTarget& Game, const TCHAR* Key,
        const FPointSpec& Spec, FVector2D& OutPixel, TSharedPtr<FJsonObject>& OutTarget)
    {
        if (!Spec.bActor)
        {
            OutPixel = Spec.Pixel;
            return true;
        }
        if (!Game.PlayerController)
        {
            Ctx.SendError(ErrorCodes::ERR_PLAYER_NOT_FOUND,
                FString::Printf(TEXT("%s names an actor, but the selected PIE world has no local player controller to project it through."), Key));
            return false;
        }
        AActor* Actor = nullptr;
        if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, Game.World, Spec.Actor, Actor))
        {
            return false;
        }
        FVector WorldPoint;
        if (!ActorAimPoint(Ctx, Actor, Spec.Component, WorldPoint))
        {
            return false;
        }
        if (!Game.PlayerController->ProjectWorldLocationToScreen(WorldPoint, OutPixel, /*bPlayerViewportRelative*/ false))
        {
            Ctx.SendError(ErrorCodes::ERR_OUT_OF_BOUNDS,
                FString::Printf(TEXT("%s actor '%s' is behind the player's camera, so it has no viewport pixel to aim at."), Key, *Actor->GetName()));
            return false;
        }
        OutTarget = MakeShared<FJsonObject>();
        OutTarget->SetStringField(TEXT("actor"), Actor->GetPathName());
        if (!Spec.Component.IsEmpty())
        {
            OutTarget->SetStringField(TEXT("component"), Spec.Component);
        }
        OutTarget->SetArrayField(TEXT("world_point"), {
            MakeShared<FJsonValueNumber>(WorldPoint.X), MakeShared<FJsonValueNumber>(WorldPoint.Y), MakeShared<FJsonValueNumber>(WorldPoint.Z) });
        return true;
    }

    FIntPoint RoundPoint(const FVector2D& Point)
    {
        return FIntPoint(FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y));
    }
}

using namespace DriveOsGestureHandlerLocal;

REGISTER_RPC_HANDLER("drive.os_gesture", "drive",
    "Real-pointer (os_input) click or drag at a PIE game-viewport pixel or world actor, with button, hold time and motion while pressed, paced across engine frames.",
    RPC_PARAMS(
        RPC_PARAM_REQ("at", "object", "Press point: {x, y} in PIE game-viewport pixels (origin top-left, FViewport size), or {actor, component?} - an actor in the PIE world (label, name or path), aimed at its bounds center (or the named component's) projected through the player's view."),
        RPC_PARAM_OPT("to", "object", "Release point, same forms as at: the pointer moves there while pressed (a drag), after any waypoints. Omitted: release at the last waypoint, or at the press point."),
        RPC_PARAM_OPT("waypoints", "array", "Points visited while pressed, in order: [{dx, dy}, ...] viewport-pixel offsets from the press point, each reached by interpolated motion (an unsteady click, a curved drag). At most 64."),
        RPC_PARAM_DEF("button", "string", "Mouse button: left | right | middle (default left).", "left"),
        RPC_PARAM_DEF("hold_ms", "integer", "Ms the button is held before the pointer moves on (or before the release when nothing moves), 0-10000 (default 80). 0 with no motion releases in the same frame as the press.", "80"),
        DRIVE_WORLD_SELECTOR_PARAM
    ))
{
    TSharedPtr<FJsonObject> AtObj;
    if (!Ctx.RequireObject(TEXT("at"), AtObj)) return true;

    const FString ButtonToken = Ctx.GetString(TEXT("button"), TEXT("left"));
    if (!ButtonToken.Equals(TEXT("left"), ESearchCase::IgnoreCase) && !ButtonToken.Equals(TEXT("right"), ESearchCase::IgnoreCase)
        && !ButtonToken.Equals(TEXT("middle"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("button must be left, right or middle; got '%s'."), *ButtonToken));
        return true;
    }
    const EDriveMouseButton Button = FDriveInput::ParseMouseButton(ButtonToken);

    const int32 HoldMs = Ctx.GetInt(TEXT("hold_ms"), FDriveOsGesture::DefaultHoldMs);
    if (HoldMs < 0 || HoldMs > 10000)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("hold_ms must be 0-10000; got %d."), HoldMs));
        return true;
    }

    FPointSpec At;
    if (!ParsePointSpec(Ctx, TEXT("at"), AtObj, At)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const TSharedPtr<FJsonObject> ToObj = Ctx.GetObject(TEXT("to"));
    FPointSpec To;
    const bool bHasTo = Payload.IsValid() && Payload->HasField(TEXT("to"));
    if (bHasTo && !ParsePointSpec(Ctx, TEXT("to"), ToObj, To)) return true;

    const TArray<TSharedPtr<FJsonValue>>* PathArr = Ctx.GetArray(TEXT("waypoints"));
    if (!PathArr && Payload.IsValid() && Payload->HasField(TEXT("waypoints")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("waypoints must be an array of {dx, dy} offsets."));
        return true;
    }
    TArray<FVector2D> Offsets;
    if (PathArr)
    {
        if (PathArr->Num() > 64)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("waypoints has %d points; at most 64 are accepted."), PathArr->Num()));
            return true;
        }
        for (int32 Index = 0; Index < PathArr->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* PointObj = nullptr;
            double Dx = 0.0;
            double Dy = 0.0;
            if (!(*PathArr)[Index].IsValid() || !(*PathArr)[Index]->TryGetObject(PointObj)
                || (*PointObj)->Values.Num() != 2
                || !(*PointObj)->TryGetNumberField(TEXT("dx"), Dx) || !(*PointObj)->TryGetNumberField(TEXT("dy"), Dy))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("waypoints[%d] must be {dx, dy}: a viewport-pixel offset from the press point."), Index));
                return true;
            }
            Offsets.Add(FVector2D(Dx, Dy));
        }
    }

    FDriveGameInputTarget Game;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FDriveGameInput::ResolveTarget(Ctx.GetString(TEXT("world")), Game, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    FString Unavailable;
    if (!FDriveOsInput::IsAvailable(Unavailable))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, Unavailable);
        return true;
    }

    const TSharedPtr<SViewport> ViewportWidget = Game.ViewportClient->GetGameViewportWidget();
    const FIntPoint ViewportSize = Game.ViewportClient->Viewport ? Game.ViewportClient->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
    const FGeometry Geometry = ViewportWidget.IsValid() ? ViewportWidget->GetTickSpaceGeometry() : FGeometry();
    const FVector2D WidgetPos(Geometry.GetAbsolutePosition());
    const FVector2D WidgetSize(Geometry.GetAbsoluteSize());
    if (!ViewportWidget.IsValid() || ViewportSize.X <= 0 || ViewportSize.Y <= 0 || WidgetSize.X <= 0.0 || WidgetSize.Y <= 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_VIEWPORT_NOT_AVAILABLE,
            TEXT("The selected PIE world's game viewport has no widget or no measured size yet (not painted); retry after a frame."));
        return true;
    }

    FVector2D PressPixel;
    TSharedPtr<FJsonObject> AtTarget;
    if (!ResolvePixel(Ctx, Game, TEXT("at"), At, PressPixel, AtTarget)) return true;
    if (PressPixel.X < 0.0 || PressPixel.Y < 0.0 || PressPixel.X >= ViewportSize.X || PressPixel.Y >= ViewportSize.Y)
    {
        TSharedPtr<FJsonObject> Details = PointJson(PressPixel.X, PressPixel.Y);
        Details->SetNumberField(TEXT("viewport_width"), ViewportSize.X);
        Details->SetNumberField(TEXT("viewport_height"), ViewportSize.Y);
        Ctx.SendError(ErrorCodes::ERR_OUT_OF_BOUNDS,
            FString::Printf(TEXT("The press point (%.0f, %.0f) is outside the %dx%d game viewport, so the press would not reach the game."),
                PressPixel.X, PressPixel.Y, ViewportSize.X, ViewportSize.Y),
            Details);
        return true;
    }

    TArray<FVector2D> PressedPixels;
    for (const FVector2D& Offset : Offsets)
    {
        PressedPixels.Add(PressPixel + Offset);
    }
    TSharedPtr<FJsonObject> ToTarget;
    if (bHasTo)
    {
        FVector2D ToPixel;
        if (!ResolvePixel(Ctx, Game, TEXT("to"), To, ToPixel, ToTarget)) return true;
        PressedPixels.Add(ToPixel);
    }
    const FVector2D ReleasePixel = PressedPixels.Num() > 0 ? PressedPixels.Last() : PressPixel;

    const auto ToScreen = [&](const FVector2D& Pixel)
    {
        return RoundPoint(FDriveOsGesture::ViewportPixelToScreen(Pixel, ViewportSize, WidgetPos, WidgetSize));
    };
    const FIntPoint PressScreen = ToScreen(PressPixel);
    TArray<FIntPoint> PressedScreen;
    for (const FVector2D& Pixel : PressedPixels)
    {
        PressedScreen.Add(ToScreen(Pixel));
    }

    // The same two gates as os_input on drive.click: another window of this editor on top of the
    // viewport at the press point (Slate's window order), or another process's X window there.
    // Only the press is gated: once a button is down, X sends the motion and the release to the
    // window that took the press.
    const TSharedPtr<SWindow> ViewportWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
    const TSharedPtr<SWindow> Top = FDriveInput::TopWindowAtPoint(FVector2D(PressScreen));
    if (ViewportWindow.IsValid() && Top.IsValid() && Top != ViewportWindow)
    {
        TSharedPtr<FJsonObject> Details = PointJson(PressScreen.X, PressScreen.Y);
        Details->SetStringField(TEXT("occluding_window"), Top->GetTitle().ToString());
        Details->SetStringField(TEXT("occluding_window_type"), FDriveEditorChrome::WindowTypeToString(Top->GetType()));
        Ctx.SendError(ErrorCodes::ERR_TARGET_OCCLUDED,
            FString::Printf(TEXT("The game viewport is covered at (%d, %d) by window '%s', which would take the press. Nothing was injected; move or close it (drive.list_windows), then retry."),
                PressScreen.X, PressScreen.Y, *Top->GetTitle().ToString()),
            Details);
        return true;
    }
    FDriveOsInput::FForeignWindow Foreign;
    if (FDriveOsInput::FindForeignWindowAt(FVector2D(PressScreen), Foreign))
    {
        TSharedPtr<FJsonObject> Details = PointJson(PressScreen.X, PressScreen.Y);
        Details->SetStringField(TEXT("occluding_window"), Foreign.Title);
        Details->SetNumberField(TEXT("occluding_window_id"), static_cast<double>(Foreign.WindowId));
        Details->SetNumberField(TEXT("occluding_pid"), Foreign.Pid);
        Ctx.SendError(ErrorCodes::ERR_TARGET_OCCLUDED,
            FString::Printf(TEXT("The press point (%d, %d) is covered by X window 0x%llx '%s' (pid %u), which is not this editor's and would receive the real input. Nothing was injected."),
                PressScreen.X, PressScreen.Y, Foreign.WindowId, *Foreign.Title, Foreign.Pid),
            Details);
        return true;
    }

    // Independent evidence of what the press point hits in the world, read before anything moves.
    TSharedPtr<FJsonValue> HitActor = MakeShared<FJsonValueNull>();
    if (Game.PlayerController)
    {
        FHitResult Hit;
        if (Game.PlayerController->GetHitResultAtScreenPosition(PressPixel, ECC_Visibility, /*bTraceComplex*/ false, Hit) && Hit.GetActor())
        {
            HitActor = MakeShared<FJsonValueString>(Hit.GetActor()->GetPathName());
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("input_path"), FDriveOsInput::InputPathLabel());
    Resp->SetStringField(TEXT("button"), ButtonToken.ToLower());
    Resp->SetNumberField(TEXT("hold_ms"), HoldMs);
    TSharedPtr<FJsonObject> PressJson = PointJson(PressPixel.X, PressPixel.Y);
    PressJson->SetObjectField(TEXT("screen"), PointJson(PressScreen.X, PressScreen.Y));
    if (AtTarget.IsValid())
    {
        PressJson->SetObjectField(TEXT("target"), AtTarget);
    }
    PressJson->SetField(TEXT("hit_actor"), HitActor);
    Resp->SetObjectField(TEXT("press"), PressJson);
    const FIntPoint ReleaseScreen = PressedScreen.Num() > 0 ? PressedScreen.Last() : PressScreen;
    TSharedPtr<FJsonObject> ReleaseJson = PointJson(ReleasePixel.X, ReleasePixel.Y);
    ReleaseJson->SetObjectField(TEXT("screen"), PointJson(ReleaseScreen.X, ReleaseScreen.Y));
    if (ToTarget.IsValid())
    {
        ReleaseJson->SetObjectField(TEXT("target"), ToTarget);
    }
    Resp->SetObjectField(TEXT("release"), ReleaseJson);

    const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
    FDriveOsGesture::Start(PressScreen, PressedScreen, Button, HoldMs, FDriveOsGesture::DefaultSegmentMs,
        [Token, Resp](bool bOk, const FDriveInjectFailure& Failure, const FDriveOsGesture::FResult& Result)
        {
            if (!bOk)
            {
                Token->SendError(Failure.Code, Failure.Message, Failure.Details);
                return;
            }
            Resp->SetNumberField(TEXT("press_frame"), static_cast<double>(Result.PressFrame));
            Resp->SetNumberField(TEXT("release_frame"), static_cast<double>(Result.ReleaseFrame));
            Resp->SetNumberField(TEXT("elapsed_ms"), FMath::RoundToDouble(Result.ElapsedMs));
            Resp->SetObjectField(TEXT("pointer_after"), PointJson(Result.PointerAfter.X, Result.PointerAfter.Y));
            Resp->SetBoolField(TEXT("pointer_on_release"), Result.bPointerOnRelease);
            Token->SendSuccess(Resp);
        });
    return true;
}
