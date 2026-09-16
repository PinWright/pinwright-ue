// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"

#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"
#include "Utils/ActorUtils.h"
// The readback preamble and the frame-state blocks shared with editor.screenshot /
// render.capture_open_level.
#include "Utils/CaptureReadinessGate.h"
#include "Utils/OnScreenMessageSurvey.h"
#include "Utils/ScreenshotUtils.h"
#include "WidgetBlueprint.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"

#if __has_include("Factories/WidgetBlueprintFactory.h")
#  include "Factories/WidgetBlueprintFactory.h"
#endif

namespace
{
    // Resolve the world whose live UserWidgets the ui.* runtime mutators should
    // target. The painted HUD that ui.create_hud adds and that ui.screenshot /
    // widget.describe(capture_source=live) render lives in the PIE viewport world,
    // so writes MUST resolve PIE-first — never editor-world-first, which would let
    // a preview/editor-spawned same-class instance intercept the write and report
    // a silent success-with-no-effect (B-set-widget-text-hits-wrong-instance).
    // Returns nullptr only when no editor exists at all.
    UWorld* ResolveRuntimeWidgetWorld()
    {
        FString ResolvedMode;
        return McpActorUtils::ResolveQueryWorld(TEXT("auto"), ResolvedMode);
    }

    // Enumerate the live UserWidgets of the PIE-first resolved world. Restricting
    // the candidate set to the rendered world is what makes the write land on the
    // painted instance instead of an editor-world decoy.
    void CollectRuntimeUserWidgets(TArray<UUserWidget*>& OutWidgets)
    {
        if (UWorld* World = ResolveRuntimeWidgetWorld())
        {
            UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, OutWidgets, UUserWidget::StaticClass(), false);
        }
    }

    // Named removal is stricter than the setter helpers: only top-level widgets
    // currently painted by the active runtime viewport, with an owning player,
    // are eligible. This prevents an editor preview or unowned transient from
    // satisfying a short-name lookup.
    void CollectRuntimeViewportUserWidgets(UWorld* RuntimeWorld, TArray<UUserWidget*>& OutWidgets)
    {
        OutWidgets.Reset();
        if (!RuntimeWorld)
        {
            return;
        }

        UWidgetBlueprintLibrary::GetAllWidgetsOfClass(
            RuntimeWorld, OutWidgets, UUserWidget::StaticClass(), true);
        for (int32 Index = OutWidgets.Num() - 1; Index >= 0; --Index)
        {
            UUserWidget* Widget = OutWidgets[Index];
            if (!Widget || Widget->GetWorld() != RuntimeWorld || !Widget->IsInViewport() || !Widget->GetOwningPlayer())
            {
                OutWidgets.RemoveAtSwap(Index);
            }
        }
    }

    void SetRuntimeWidgetIdentity(const TSharedPtr<FJsonObject>& Result, UUserWidget* Widget)
    {
        if (!Widget)
        {
            return;
        }

        Result->SetStringField(TEXT("widgetName"), Widget->GetName());
        Result->SetStringField(TEXT("objectPath"), Widget->GetPathName());
        Result->SetStringField(TEXT("path"), Widget->GetPathName());
        Result->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());

        if (UWorld* WidgetWorld = Widget->GetWorld())
        {
            Result->SetStringField(TEXT("world"), WidgetWorld->GetName());
            Result->SetStringField(TEXT("worldPath"), WidgetWorld->GetPathName());
        }

        if (APlayerController* OwningPlayer = Widget->GetOwningPlayer())
        {
            Result->SetStringField(TEXT("player"), OwningPlayer->GetName());
            Result->SetStringField(TEXT("playerPath"), OwningPlayer->GetPathName());
        }
    }

    // Find the first live child widget of type TChild named Key across the PIE-first
    // resolved world's UserWidgets. Sets OutOwner to the UserWidget that owns the
    // match. Shared by ui.set_widget_text / ui.set_widget_image so the candidate
    // iteration and first-match-wins policy live in one place.
    template <class TChild>
    TChild* FindRuntimeChildByKey(const FString& Key, UUserWidget*& OutOwner)
    {
        OutOwner = nullptr;
        TArray<UUserWidget*> Widgets;
        CollectRuntimeUserWidgets(Widgets);
        for (UUserWidget* Widget : Widgets)
        {
            if (TChild* Child = Cast<TChild>(Widget->GetWidgetFromName(FName(*Key))))
            {
                OutOwner = Widget;
                return Child;
            }
        }
        return nullptr;
    }

    // Visibility's variant: the key may name the top-level UserWidget itself OR a
    // named child of any type, so it returns the base UWidget*.
    UWidget* FindRuntimeWidgetByKey(const FString& Key, UUserWidget*& OutOwner)
    {
        OutOwner = nullptr;
        TArray<UUserWidget*> Widgets;
        CollectRuntimeUserWidgets(Widgets);
        for (UUserWidget* Widget : Widgets)
        {
            if (Widget->GetName() == Key)
            {
                OutOwner = Widget;
                return Widget;
            }
            if (UWidget* Child = Widget->GetWidgetFromName(FName(*Key)))
            {
                OutOwner = Widget;
                return Child;
            }
        }
        return nullptr;
    }

    // Echo the mutated owning UserWidget so callers can confirm the painted widget
    // was hit. Always called with a non-null owner from a successful match.
    void EchoOwningUserWidget(const TSharedPtr<FJsonObject>& Result, UUserWidget* Owner)
    {
        Result->SetStringField(TEXT("owning_user_widget"), Owner->GetName());
    }
}

// ---- ui.screenshot ----
REGISTER_RPC_HANDLER("ui.screenshot", "ui", "Capture a screenshot of the active viewport. Near-duplicate of editor.screenshot — prefer editor.screenshot which is the canonical method; this one exists for symmetry with the ui.* runtime ops.",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "filepath", "Save path for the screenshot"),
        RPC_PARAM_OPT("filename", "filepath", "Filename for the screenshot"),
        RPC_PARAM_OPT("returnBase64", "boolean", "Whether to return base64-encoded image data (default true)")
    ))
{
    // Delegate path+filename composition to MakeUiScreenshotPath (full contract in
    // ScreenshotUtils.h): .png is appended only when absent, so a caller-supplied ".png"
    // is no longer doubled to ".png.png" (E-ui-screenshot-doubles-png-extension). Filename
    // is an out-param receiving the resolved basename echoed in the response below.
    FString Filename;
    const FString FullPath = PinWrightScreenshotUtils::MakeUiScreenshotPath(
        Ctx.GetString(TEXT("path")), Ctx.GetString(TEXT("filename")), Filename);

    const bool bReturnBase64 = Ctx.GetBool(TEXT("returnBase64"), true);

    // Capture the active game viewport through the shared game-viewport->PNG path
    // (the same one editor.screenshot's PIE branch uses), then echo the encoded
    // bytes back base64 below.
    int32 Width = 0, Height = 0;
    FString CaptureError;
    TArray<uint8> PngData;

    // The same readback preamble editor.screenshot's PIE branch runs, immediately before the same
    // shared capture call: drain the shader queue so the frame cannot photograph default materials,
    // then let CaptureGameViewportToPngFile flush before its readback.
    const PinWrightCaptureReadiness::FReadinessResult Readiness =
        PinWrightCaptureReadiness::DrainBeforeReadback();
    if (Readiness.ShouldRefuseReadback())
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        PinWrightCaptureReadiness::AddReadinessFields(Readiness, Details);
        Ctx.SendError(ErrorCodes::ERR_CAPTURE_NOT_READY,
            PinWrightCaptureReadiness::MakeRefusalMessage(Readiness), Details);
        return true;
    }

    PinWrightScreenshotUtils::FGameViewportCaptureMetadata Metadata;
    if (!PinWrightScreenshotUtils::CaptureGameViewportToPngFile(
            FullPath, Width, Height, CaptureError, &PngData, nullptr, &Metadata))
    {
        const TCHAR* Message =
            CaptureError == TEXT("NO_VIEWPORT")   ? TEXT("No game viewport available")
            : CaptureError == TEXT("CAPTURE_FAILED") ? TEXT("Failed to read viewport pixels")
            : CaptureError == TEXT("BLANK_CAPTURE")
                ? TEXT("The viewport read back as an entirely empty frame — nothing has been "
                       "rendered into it yet. Start or step PIE so the viewport presents at "
                       "least one frame, then retry. No file was written.")
            : TEXT("Failed to save screenshot");
        Ctx.SendError(CaptureError, Message);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("screenshotPath"), FullPath);
    Result->SetStringField(TEXT("filename"), Filename);
    Result->SetNumberField(TEXT("width"), Width);
    Result->SetNumberField(TEXT("height"), Height);
    Result->SetNumberField(TEXT("sizeBytes"), PngData.Num());

    // Same two blocks the other two capture verbs publish, on the same `viewport` key, so a caller
    // reading a ui.screenshot response gets the same frame-state contract.
    {
        PinWrightCaptureReadiness::FReadinessResult Published = Readiness;
        Published.bReadbackFlushed = Metadata.bReadbackFlushed;
        const TSharedPtr<FJsonObject> ViewportBlock = MakeShared<FJsonObject>();
        PinWrightCaptureReadiness::AddReadinessFields(Published, ViewportBlock);
        PinWrightOnScreenMessages::AddOnScreenMessageFields(
            PinWrightOnScreenMessages::Survey(), ViewportBlock);
        Result->SetObjectField(TEXT("viewport"), ViewportBlock);
    }

    if (bReturnBase64 && PngData.Num() > 0)
    {
        FString Base64Data = FBase64::Encode(PngData);
        Result->SetStringField(TEXT("imageBase64"), Base64Data);
        Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ---- ui.create_hud ----
REGISTER_RPC_HANDLER("ui.create_hud", "ui", "Construct a UMG widget instance from a UWidgetBlueprint and add it to the player viewport at runtime. Distinct from widget.create_widget_blueprint which authors the asset; this one instantiates an existing one.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "classref", "Class path for the widget to create")
    ))
{
    FString WidgetPath = Ctx.GetString(TEXT("widgetPath"));
    // Route through the canonical resolver so a bare Blueprint asset path
    // (/Game/.../WBP_PlayerHUD.WBP_PlayerHUD) resolves to the generated UClass
    // without the caller having to append the _C generated-class suffix — matching
    // the resolver contract every other class-path slot in the API already honors.
    // ResolveUClass returns any UClass*; guard that it is a UUserWidget subclass so
    // the downstream CreateWidget<UUserWidget> cast stays type-safe.
    UClass* WidgetClass = ResolveUClass(WidgetPath);
    if (WidgetClass && !WidgetClass->IsChildOf(UUserWidget::StaticClass()))
    {
        WidgetClass = nullptr;
    }

    // Distinguish a failed class resolution from a missing viewport instead of
    // collapsing both into CLASS_NOT_FOUND: the former means the widgetPath is
    // wrong, the latter means PIE isn't running. The split also matches the
    // NO_VIEWPORT code the other ui.* handlers in this file already return, and
    // lets the regression test assert resolution succeeded by observing a
    // non-CLASS_NOT_FOUND error even though ui.create_hud can't add to a
    // viewport headlessly.
    if (!WidgetClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND, FString::Printf(TEXT("Failed to load widget class: %s"), *WidgetPath));
        return true;
    }

    if (!GEngine || !GEngine->GameViewport)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_VIEWPORT, TEXT("No game viewport available (is PIE running?)"));
        return true;
    }

    UWorld* World = GEngine->GameViewport->GetWorld();
    if (World)
    {
        UUserWidget* Widget = CreateWidget<UUserWidget>(World, WidgetClass);
        if (Widget)
        {
            Widget->AddToViewport();
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("widgetName"), Widget->GetName());
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create widget"));
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No world context found (is PIE running?)"));
    }
    return true;
}

// ---- ui.set_widget_text ----
REGISTER_RPC_HANDLER("ui.set_widget_text", "ui", "Set the Text property on a runtime UTextBlock widget by name (live in the viewport). For asset-side text changes use property.set on the widget blueprint's CDO.",
    RPC_PARAMS(
        RPC_PARAM_REQ("key", "string", "Name of the TextBlock widget"),
        RPC_PARAM_REQ("value", "string", "Text value to set")
    ))
{
    FString Key = Ctx.GetString(TEXT("key"));
    FString Value = Ctx.GetString(TEXT("value"));

    // Resolve PIE-first so the write lands on the painted/rendered instance, not
    // an editor-world decoy that would silently absorb the SetText.
    UUserWidget* OwningWidget = nullptr;
    if (UTextBlock* MatchedTextBlock = FindRuntimeChildByKey<UTextBlock>(Key, OwningWidget))
    {
        MatchedTextBlock->SetText(FText::FromString(Value));
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("key"), Key);
        Result->SetStringField(TEXT("value"), Value);
        EchoOwningUserWidget(Result, OwningWidget);
        Ctx.SendSuccess(Result);
    }
    else
    {
        // No live instance in the rendered world matched — error rather than
        // silently succeeding on an editor-world decoy.
        Ctx.SendError(ErrorCodes::ERR_WIDGET_NOT_FOUND,
            FString::Printf(TEXT("No live TextBlock '%s' found in the runtime widget world (is the painted HUD added to the PIE viewport?)"), *Key));
    }
    return true;
}

// ---- ui.set_widget_image ----
REGISTER_RPC_HANDLER("ui.set_widget_image", "ui", "Set the Brush texture on a runtime UImage widget by name. Affects only the live viewport instance; not persisted to the asset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("key", "string", "Name of the Image widget"),
        RPC_PARAM_REQ("texturePath", "path", "Path to the texture asset")
    ))
{
    FString Key = Ctx.GetString(TEXT("key"));
    FString TexturePath = Ctx.GetString(TEXT("texturePath"));

    UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *TexturePath);
    if (!Texture)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Failed to load texture"));
        return true;
    }

    // Resolve PIE-first so the brush lands on the painted instance, mirroring
    // ui.set_widget_text rather than TObjectIterator's non-deterministic order.
    UUserWidget* OwningWidget = nullptr;
    if (UImage* MatchedImage = FindRuntimeChildByKey<UImage>(Key, OwningWidget))
    {
        MatchedImage->SetBrushFromTexture(Texture);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("key"), Key);
        EchoOwningUserWidget(Result, OwningWidget);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_WIDGET_NOT_FOUND,
            FString::Printf(TEXT("No live Image widget '%s' found in the runtime widget world (is the painted HUD added to the PIE viewport?)"), *Key));
    }
    return true;
}

// ---- ui.set_widget_visibility ----
REGISTER_RPC_HANDLER("ui.set_widget_visibility", "ui", "Set the ESlateVisibility on a runtime widget by name. Pass `visibility` as one of Visible / Collapsed / Hidden / HitTestInvisible / SelfHitTestInvisible for the full enum; the `visible` boolean is a back-compat shorthand mapping true->Visible, false->Collapsed (ignored when `visibility` is given). Echoes the resolved `visibility` enum string.",
    RPC_PARAMS(
        RPC_PARAM_REQ("key", "string", "Name of the widget"),
        RPC_PARAM_OPT("visibility", "string", "ESlateVisibility state: Visible / Collapsed / Hidden / HitTestInvisible / SelfHitTestInvisible. Takes precedence over `visible`."),
        RPC_PARAM_OPT("visible", "boolean", "Back-compat shorthand: true->Visible, false->Collapsed (default true). Ignored when `visibility` is supplied.")
    ))
{
    FString Key = Ctx.GetString(TEXT("key"));

    // Resolve the requested ESlateVisibility. The full enum string `visibility`
    // takes precedence; an unknown string is rejected (never silently coerced to
    // a bool). When absent, fall back to the `visible` bool shorthand
    // (true->Visible, false->Collapsed) for back-compat.
    ESlateVisibility TargetVisibility = ESlateVisibility::Visible;
    const FString VisibilityStr = Ctx.GetString(TEXT("visibility"));
    if (!VisibilityStr.IsEmpty())
    {
        if (!WidgetAuthoringHelpers::TryParseVisibility(VisibilityStr, TargetVisibility))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_VISIBILITY,
                FString::Printf(TEXT("Unknown visibility '%s'. Valid values: Visible, Collapsed, Hidden, HitTestInvisible, SelfHitTestInvisible."), *VisibilityStr));
            return true;
        }
    }
    else
    {
        const bool bVisible = Ctx.GetBool(TEXT("visible"), true);
        TargetVisibility = bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
    }

    // Resolve PIE-first so visibility flips the painted instance, not an
    // editor-world / non-rendered widget that TObjectIterator might reach first.
    UUserWidget* OwningWidget = nullptr;
    if (UWidget* MatchedWidget = FindRuntimeWidgetByKey(Key, OwningWidget))
    {
        MatchedWidget->SetVisibility(TargetVisibility);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("key"), Key);
        // Echo the resolved enum so the caller can confirm the exact state applied,
        // plus the bool for back-compat (true only for the fully-Visible state).
        Result->SetStringField(TEXT("visibility"), WidgetAuthoringHelpers::VisibilityToString(TargetVisibility));
        Result->SetBoolField(TEXT("visible"), TargetVisibility == ESlateVisibility::Visible);
        EchoOwningUserWidget(Result, OwningWidget);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_WIDGET_NOT_FOUND,
            FString::Printf(TEXT("No live widget '%s' found in the runtime widget world (is the painted HUD added to the PIE viewport?)"), *Key));
    }
    return true;
}

// ---- ui.remove_widget_from_viewport ----
REGISTER_RPC_HANDLER("ui.remove_widget_from_viewport", "ui", "Remove a UMG widget instance from the player viewport, undoing a prior ui.create_hud. Does not destroy the widget blueprint asset.",
    RPC_PARAMS(
        RPC_PARAM_OPT("key", "string", "Name of the widget (empty = remove all)")
    ))
{
    FString Key = Ctx.GetString(TEXT("key"));

    if (Key.IsEmpty())
    {
        // Remove all user widgets
        if (GEngine && GEngine->GameViewport && GEngine->GameViewport->GetWorld())
        {
            TArray<UUserWidget*> Widgets;
            UWidgetBlueprintLibrary::GetAllWidgetsOfClass(
                GEngine->GameViewport->GetWorld(), Widgets, UUserWidget::StaticClass(), true);
            for (UUserWidget* W : Widgets)
            {
                W->RemoveFromParent();
            }
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("removedCount"), Widgets.Num());
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_NO_VIEWPORT, TEXT("No game viewport available"));
        }
    }
    else
    {
        if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->GetWorld())
        {
            Ctx.SendError(ErrorCodes::ERR_NO_VIEWPORT, TEXT("No game viewport available"));
            return true;
        }

        UWorld* ViewportWorld = GEngine->GameViewport->GetWorld();
        UWorld* RuntimeWorld = ResolveRuntimeWidgetWorld();
        if (!RuntimeWorld || RuntimeWorld != ViewportWorld)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_VIEWPORT, TEXT("The runtime widget world does not match the active game viewport"));
            return true;
        }

        TArray<UUserWidget*> Widgets;
        CollectRuntimeViewportUserWidgets(RuntimeWorld, Widgets);

        TArray<UUserWidget*> Matches;
        FString MatchedBy;
        for (UUserWidget* Widget : Widgets)
        {
            if (Widget && Widget->GetPathName() == Key)
            {
                Matches.Add(Widget);
            }
        }
        if (Matches.Num() > 0)
        {
            MatchedBy = TEXT("objectPath");
        }
        else
        {
            for (UUserWidget* Widget : Widgets)
            {
                if (Widget && Widget->GetName() == Key)
                {
                    Matches.Add(Widget);
                }
            }
            MatchedBy = TEXT("objectName");
        }

        if (Matches.Num() > 1)
        {
            TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
            ErrorResult->SetStringField(TEXT("requestedName"), Key);
            ErrorResult->SetStringField(TEXT("matchedBy"), MatchedBy);
            ErrorResult->SetNumberField(TEXT("candidateCount"), Matches.Num());

            TArray<TSharedPtr<FJsonValue>> Candidates;
            for (UUserWidget* Widget : Matches)
            {
                TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
                SetRuntimeWidgetIdentity(Candidate, Widget);
                Candidate->SetStringField(TEXT("name"), Widget->GetName());
                Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
            }
            ErrorResult->SetArrayField(TEXT("candidates"), Candidates);

            Ctx.SendError(
                ErrorCodes::ERR_AMBIGUOUS_ACTOR_NAME,
                FString::Printf(TEXT("Widget '%s' matches %d live viewport instances; reissue with a candidate objectPath"), *Key, Matches.Num()),
                ErrorResult);
            return true;
        }

        if (Matches.Num() == 1)
        {
            UUserWidget* Widget = Matches[0];
            Widget->RemoveFromParent();
            if (Widget->IsInViewport())
            {
                TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
                SetRuntimeWidgetIdentity(ErrorResult, Widget);
                Ctx.SendError(ErrorCodes::ERR_REMOVE_FAILED, TEXT("The selected widget remained attached to the viewport"), ErrorResult);
                return true;
            }

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("key"), Key);
            SetRuntimeWidgetIdentity(Result, Widget);
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_WIDGET_NOT_FOUND, FString::Printf(TEXT("Live viewport widget '%s' not found in the active runtime world"), *Key));
        }
    }
    return true;
}
