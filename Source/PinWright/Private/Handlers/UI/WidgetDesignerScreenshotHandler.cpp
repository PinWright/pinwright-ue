// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetDesignerCaptureUtil.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Utils/ScreenshotUtils.h"

#include "Handlers/UI/WidgetDesignerCaptureInternal.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UnrealClient.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

using namespace WidgetAuthoringHelpers;
using WidgetDesignerCaptureInternal::FindWidgetEditorHostWindow;
using WidgetDesignerCaptureInternal::FindWidgetEditorHostWindowWithRetry;
using WidgetDesignerCaptureInternal::OpenAndInvokeDesigner;
using WidgetDesignerCaptureInternal::PumpDesignerSlate;

namespace
{
    void ParseStringArrayParam(FHandlerContext& Ctx, const TCHAR* ParamName, TArray<FString>& OutNames)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = Ctx.GetArray(ParamName);
        if (!Arr) return;
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            FString Entry;
            if (V.IsValid() && V->TryGetString(Entry) && !Entry.IsEmpty())
            {
                OutNames.Add(MoveTemp(Entry));
            }
        }
    }

    FString FormatDesignerCaptureError(const FString& ErrorCode, const FString& WidgetPath)
    {
        if (ErrorCode == ErrorCodes::ERR_EDITOR_NOT_READY)
        {
            return FString::Printf(
                TEXT("Widget Blueprint Designer is still initializing or loading; retry after the startup map completes: %s"),
                *WidgetPath);
        }
        return FString::Printf(
            TEXT("Widget Blueprint Designer unavailable (%s) for: %s"),
            *ErrorCode,
            *WidgetPath);
    }

    // Maps WidgetDesignerCaptureUtil error codes to widget.screenshot_designer's public
    // error contract. HandlerCode == nullptr means passthrough (use the util's code).
    // bAppendWidgetPath == false means MessageFmt is a static string (no formatting).
    struct FCaptureErrorMapping
    {
        const TCHAR* UtilCode;
        const TCHAR* HandlerCode;
        const TCHAR* MessageFmt;
        bool bAppendWidgetPath;
    };
    // MessageFmt uses FString::Format placeholder {0} (not printf %s) because UE 5.6's
    // Printf hardening (TCheckedFormatStringPrivate) requires a compile-time literal
    // format and rejects runtime const TCHAR* fields.
    static constexpr FCaptureErrorMapping GCaptureErrorMappings[] = {
        { TEXT("PREVIEW_NOT_FOUND"),        nullptr,                TEXT("Designer preview UserWidget not available for: {0}"),                  true  },
        { TEXT("PREVIEW_BOUNDS_NOT_FOUND"), nullptr,                TEXT("Widget Blueprint Designer preview bounds could not be resolved for: {0}"), true },
        { TEXT("PREVIEW_ZERO_SIZE"),        TEXT("INVALID_STATE"),  TEXT("Designer preview reports zero size for: {0}"),                         true  },
        { TEXT("RT_CREATE_FAILED"),         TEXT("CAPTURE_FAILED"), TEXT("Failed to create render target for preview capture: {0}"),             true  },
        { TEXT("READ_PIXELS_FAILED"),       TEXT("CAPTURE_FAILED"), TEXT("Failed to read pixels from preview render target: {0}"),               true  },
        { TEXT("ENCODE_FAILED"),            nullptr,                TEXT("Failed to encode Widget Blueprint Designer screenshot as PNG"),        false },
        { TEXT("EDITOR_NOT_READY"),         nullptr,                TEXT("Widget Blueprint Designer is still initializing or loading; retry after the startup map completes: {0}"), true },
    };
}

REGISTER_RPC_HANDLER("widget.screenshot_designer", "widget",
    "Capture the Widget Blueprint Designer preview or containing editor window as a PNG under Saved/Screenshots/WidgetDesigner.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "path", "Widget Blueprint asset path, e.g. /Game/UI/WBP_Menu."),
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/WidgetDesigner. The .png extension is appended if missing."),
        RPC_PARAM_OPT("target", "string", "Capture target: 'preview' (default) or 'window'."),
        RPC_PARAM_OPT("showOnly", "array", "Names of widgets that should remain designer-eye visible. Their ancestors are also kept visible; every other widget under the root is designer-eye-hidden for the capture only. Reverted automatically."),
        RPC_PARAM_OPT("hide", "array", "Names of widgets to designer-eye-hide on top of their saved state, for this capture only. Reverted automatically."),
        RPC_PARAM_OPT("visibilityOverrides", "object", "Per-widget runtime Visibility override (e.g. {\"Foo\":\"Visible\",\"Bar\":\"Collapsed\"}) applied to the preview widget for the capture only. Reverted automatically."),
        RPC_PARAM_DEF("max_size", "integer", "Maximum pixel size on the longer axis of the output. The shorter axis is derived to preserve aspect ratio. Applies only to target=preview.", "1024"),
        RPC_PARAM_OPT("closeAfterCapture", "boolean", "Close the Widget Blueprint editor after the capture. Defaults to TRUE, which closes only a Designer this call opened; pass true explicitly to close one that was already open, or false to leave it open for an iteration loop. Same three-state rule and the same deferred close as render.capture_asset_preview: the close is QUEUED onto the next editor tick rather than run inside this call, so the response reports assetEditorClosed:false with assetEditorCloseDeferred:true and the window is gone a tick later. Leaving a Designer open across a later blueprint.compile / blueprint.compile_bpir of the same widget used to kill the editor; that is guarded on the compile side now, but a capture verb still owes the caller the state it found.")
    ))
{
    const FString WidgetPath = Ctx.GetString(TEXT("widgetPath"));
    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("widgetPath required"));
        return true;
    }

    FString Target = Ctx.GetString(TEXT("target")).ToLower();
    if (Target.IsEmpty())
    {
        Target = TEXT("preview");
    }
    if (Target != TEXT("preview") && Target != TEXT("window"))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("target must be 'preview' or 'window'"));
        return true;
    }

    // 16384 px hard cap — beyond this the RGBA8 render target alloc exceeds 1 GB.
    // Prevents unbounded UTextureRenderTarget2D allocations when callers pass a typo or
    // attacker-supplied value. The minimum is 1px; 0 or negative values are rejected
    // because the longest-axis cap math degenerates.
    const int32 MaxSize = Ctx.GetInt(TEXT("max_size"), 1024);
    if (MaxSize <= 0 || MaxSize > 16384)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("max_size must be in (0, 16384]"));
        return true;
    }

    // Three states, not two: absent closes only a Designer this call opened, an explicit true
    // closes one the caller already had open, false leaves it. The presence test is what makes
    // the second capture of the same widget still clean up after itself.
    const bool bCloseRequestedExplicitly =
        Ctx.GetJsonValueFirstOf({ TEXT("closeAfterCapture") }).IsValid();
    const bool bCloseAfterCapture = Ctx.GetBool(TEXT("closeAfterCapture"), true);

    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
        return true;
    }

    FString ReadinessError;
    if (!WidgetDesignerCaptureInternal::QueryDesignerCaptureReadiness(ReadinessError))
    {
        Ctx.SendError(ReadinessError,
            FormatDesignerCaptureError(ReadinessError, WidgetPath));
        return true;
    }

    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem)
    {
        Ctx.SendError(TEXT("SUBSYSTEM_MISSING"), TEXT("AssetEditorSubsystem not available"));
        return true;
    }

    UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBlueprint)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), FString::Printf(TEXT("Asset is not a Widget Blueprint: %s"), *WidgetPath));
        return true;
    }

    // Opens the Designer and, on EVERY exit path including the error returns below, puts the
    // editor back the way it was found. See FScopedDesignerAssetEditor for the three-state rule
    // and why the close is queued rather than run here.
    WidgetDesignerCaptureInternal::FScopedDesignerAssetEditor ScopedDesigner(
        WidgetBlueprint, bCloseAfterCapture, bCloseRequestedExplicitly);
    if (!ScopedDesigner.IsOpen())
    {
        Ctx.SendError(TEXT("OPEN_FAILED"), FString::Printf(TEXT("Failed to open Widget Blueprint editor: %s"), *WidgetPath));
        return true;
    }

    FWidgetBlueprintEditor* WidgetEditor = FWidgetGeometryResolver::FindWidgetBlueprintEditor(
        WidgetBlueprint, true);
    if (!WidgetEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_FOUND"), FString::Printf(TEXT("Widget Blueprint editor not found after opening: %s"), *WidgetPath));
        return true;
    }

    TArray<FString> ShowOnly;
    ParseStringArrayParam(Ctx, TEXT("showOnly"), ShowOnly);

    TArray<FString> Hide;
    ParseStringArrayParam(Ctx, TEXT("hide"), Hide);

    TMap<FString, ESlateVisibility> VisibilityOverridesMap;
    if (TSharedPtr<FJsonObject> VisibilityOverridesObj = Ctx.GetObject(TEXT("visibilityOverrides")))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : VisibilityOverridesObj->Values)
        {
            FString VisStr;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(VisStr))
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                    FString::Printf(TEXT("visibilityOverrides[%s] must be a string"), *Pair.Key));
                return true;
            }

            ESlateVisibility ParsedVis;
            if (!TryParseVisibility(VisStr, ParsedVis))
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                    FString::Printf(TEXT("visibilityOverrides[%s] has unknown visibility '%s'"),
                        *Pair.Key, *VisStr));
                return true;
            }
            VisibilityOverridesMap.Add(Pair.Key, ParsedVis);
        }
    }

    const bool bAnyOverrides = ShowOnly.Num() > 0 || Hide.Num() > 0 || VisibilityOverridesMap.Num() > 0;

    // Window capture needs the Designer tab up front. Preview capture normally lets
    // the shared retry helper open it exactly once; overrides are the exception
    // because they need the live preview object before capture begins.
    const bool bDesignerMustOpenBeforeCapture = Target == TEXT("window") || bAnyOverrides;
    if (bDesignerMustOpenBeforeCapture)
    {
        FString DesignerOpenError;
        OpenAndInvokeDesigner(WidgetEditor, &DesignerOpenError);
        if (!DesignerOpenError.IsEmpty())
        {
            Ctx.SendError(DesignerOpenError,
                FormatDesignerCaptureError(DesignerOpenError, WidgetPath));
            return true;
        }
    }

    FTransientDesignerOverrides Guard;
    ON_SCOPE_EXIT
    {
        if (Guard.Entries.Num() > 0)
        {
            RevertTransientDesignerOverrides(Guard);
            if (WidgetEditor)
            {
                WidgetEditor->InvalidatePreview(true);
            }
        }
    };

    if (bAnyOverrides)
    {
        UUserWidget* PreviewRoot = WidgetEditor->GetPreview();
        if (!PreviewRoot)
        {
            Ctx.SendError(TEXT("PREVIEW_NOT_FOUND"),
                FString::Printf(TEXT("Designer preview UserWidget not available for: %s"), *WidgetPath));
            return true;
        }
        FString ApplyError;
        if (!ApplyTransientDesignerOverrides(PreviewRoot, ShowOnly, Hide, VisibilityOverridesMap, Guard, ApplyError))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), ApplyError);
            return true;
        }

        // Pump Slate so the override-flipped preview gets a paint pass before TakeScreenshot.
        WidgetEditor->InvalidatePreview(true);
        FString OverridePumpError;
        if (!PumpDesignerSlate(FindWidgetEditorHostWindow(WidgetEditor), &OverridePumpError))
        {
            const FString OverrideErrorCode = OverridePumpError.IsEmpty()
                ? FString(ErrorCodes::ERR_EDITOR_NOT_READY)
                : OverridePumpError;
            Ctx.SendError(
                OverrideErrorCode,
                FormatDesignerCaptureError(OverrideErrorCode, WidgetPath));
            return true;
        }
    }

    FWidgetDesignerPreviewTarget DesignerTarget;
    FString ResolveError;
    TSharedPtr<SWindow> EditorHostWindow;
    TArray<uint8> PngBytes;
    // Only the preview branch measures the alpha it stamps over, so only the preview branch
    // has anything to publish. The window branch stamps too (below, via ForceOpaqueAlpha on a
    // Slate back-buffer readback) but takes no pre-stamp measurement, and inventing a
    // fraction there -- or emitting a bare opaqueStamped with no number beside it -- would be
    // a field that answers a question nobody measured. bAlphaFactsMeasured is the gate; it is
    // set from the util's own report, never from the fact that this branch was taken.
    bool bAlphaFactsMeasured = false;
    bool bOpaqueStamped = false;
    double AlphaZeroFraction = 0.0;
    int32 OutWidth = 0;
    int32 OutHeight = 0;
    float OutDpiScale = 1.0f;
    FString OutPreviewName;

    if (Target == TEXT("window"))
    {
        FString WindowResolveError;
        EditorHostWindow = FindWidgetEditorHostWindowWithRetry(WidgetEditor, &WindowResolveError);
        if (!EditorHostWindow.IsValid())
        {
            if (!WindowResolveError.IsEmpty())
            {
                Ctx.SendError(WindowResolveError,
                    FormatDesignerCaptureError(WindowResolveError, WidgetPath));
                return true;
            }
            Ctx.SendError(TEXT("WINDOW_NOT_FOUND"), FString::Printf(TEXT("Widget Blueprint editor window not found for: %s"), *WidgetPath));
            return true;
        }

        // Window mode doesn't consume preview crop bounds; skip the SDesignerView lookup
        // since only Preview/DpiScale/HostWindow are reported.
        FWidgetGeometryResolver::ResolveDesignerPreviewTarget(
            WidgetBlueprint, true, DesignerTarget, &ResolveError, /*bResolveDesignerSurface=*/false);

        TArray<FColor> ColorData;
        FIntVector ImageSize(0, 0, 0);
        TSharedRef<SWidget> CaptureWidget = StaticCastSharedRef<SWidget>(EditorHostWindow.ToSharedRef());
        // Never FSlateApplication::TakeScreenshot directly: a window the pass does not draw leaves
        // the renderer armed with a pointer to this frame. Contract in ScreenshotUtils.h.
        const bool bCaptured = PinWrightScreenshotUtils::TakeSlateScreenshot(
            CaptureWidget, ColorData, ImageSize);
        if (!bCaptured || ImageSize.X <= 0 || ImageSize.Y <= 0 || ColorData.Num() == 0)
        {
            Ctx.SendError(TEXT("CAPTURE_FAILED"), FString::Printf(TEXT("Failed to capture Widget Blueprint Designer %s for: %s"), *Target, *WidgetPath));
            return true;
        }

        // Force alpha opaque (Slate may leave it non-255); contract in ScreenshotUtils.h.
        // Same window back-buffer source as EditorWindowHandlers.cpp's editor.screenshot_window
        // and FDriveEditorChrome::CaptureWindow, both of which stamp — without it this path
        // writes a near-fully-transparent PNG that reads as blank in any alpha-compositing
        // viewer (B-horizontal-orthographic-views-render-no-geometry).
        PinWrightScreenshotUtils::ForceOpaqueAlpha(ColorData);

        TArray64<uint8> PngData;
        FImageUtils::PNGCompressImageArray(ImageSize.X, ImageSize.Y,
            TArrayView64<const FColor>(ColorData.GetData(), ColorData.Num()), PngData);
        if (PngData.Num() == 0)
        {
            Ctx.SendError(TEXT("ENCODE_FAILED"), TEXT("Failed to encode Widget Blueprint Designer screenshot as PNG"));
            return true;
        }
        PngBytes.Reset(PngData.Num());
        PngBytes.Append(PngData.GetData(), PngData.Num());

        OutWidth = ImageSize.X;
        OutHeight = ImageSize.Y;
        OutDpiScale = EditorHostWindow.IsValid() ? EditorHostWindow->GetDPIScaleFactor() : DesignerTarget.DpiScale;
        if (DesignerTarget.Preview)
        {
            OutPreviewName = DesignerTarget.Preview->GetName();
        }
    }
    else
    {
        // Preview capture goes through the shared util so the asset.dump aspect path
        // produces byte-identical output without duplicating the screenshot pipeline.
        WidgetDesignerCaptureUtil::FCaptureInfo Info;
        FString CaptureError;
        if (!WidgetDesignerCaptureUtil::CapturePreviewToPng(
            WidgetBlueprint, MaxSize, PngBytes, CaptureError, &Info,
            /*bDesignerAlreadyOpen=*/bAnyOverrides))
        {
            // Map the util's stable error codes back to the handler's existing codes so
            // the public widget.screenshot_designer contract is unchanged. Table-driven
            // lookup; unmatched codes fall through to the generic "target not available"
            // message with the util's code passed through verbatim.
            FString ErrorCode = CaptureError;
            FString Message = FString::Printf(TEXT("Widget Blueprint Designer target not available for: %s"), *WidgetPath);
            for (const FCaptureErrorMapping& Map : GCaptureErrorMappings)
            {
                if (CaptureError == Map.UtilCode)
                {
                    if (Map.HandlerCode)
                    {
                        ErrorCode = Map.HandlerCode;
                    }
                    Message = Map.bAppendWidgetPath
                        ? FString::Format(Map.MessageFmt, { WidgetPath })
                        : FString(Map.MessageFmt);
                    break;
                }
            }
            Ctx.SendError(ErrorCode, Message);
            return true;
        }
        OutWidth = Info.Width;
        OutHeight = Info.Height;
        OutDpiScale = Info.DpiScale;
        OutPreviewName = Info.PreviewName;
        // Read back off the util's report rather than asserted here. If a later edit makes
        // CapturePreviewToPng stop stamping, this response says opaqueStamped:false instead
        // of continuing to promise a stamp that no longer happens -- which is the whole
        // reason FCaptureInfo carries a field instead of the handler carrying a constant.
        bAlphaFactsMeasured = true;
        bOpaqueStamped = Info.bOpaqueStamped;
        AlphaZeroFraction = Info.AlphaZeroFraction;
    }

    const FString AssetName = FPackageName::GetLongPackageAssetName(WidgetPath);
    FString Filename;
    const FString OutputPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
        Ctx.GetString(TEXT("filename")),
        FString::Printf(TEXT("%s_Designer"), AssetName.IsEmpty() ? TEXT("Widget") : *AssetName),
        TEXT("WidgetDesigner"),
        Filename);
    if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
    {
        Ctx.SendError(TEXT("SAVE_FAILED"), FString::Printf(TEXT("Failed to save Widget Blueprint Designer screenshot: %s"), *OutputPath));
        return true;
    }

    // Restored BEFORE the response is built, so the three fields below are measured rather than
    // predicted. Same vocabulary as render.capture_asset_preview's `subject` block:
    // `assetEditorClosed` is true only when the window is gone as this answers, and
    // `assetEditorCloseDeferred` is what distinguishes "queued, gone next tick" from "left open".
    const bool bAssetEditorClosed = ScopedDesigner.Restore();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), OutputPath);
    Result->SetStringField(TEXT("filename"), Filename);
    Result->SetNumberField(TEXT("width"), OutWidth);
    Result->SetNumberField(TEXT("height"), OutHeight);
    Result->SetNumberField(TEXT("sizeBytes"), PngBytes.Num());
    Result->SetStringField(TEXT("target"), Target);
    Result->SetStringField(TEXT("widgetPath"), WidgetPath);
    Result->SetStringField(TEXT("captureSource"), Target == TEXT("window") ? TEXT("editorWindow") : TEXT("designerPreview"));
    if (Target == TEXT("preview"))
    {
        // max_size only meaningful for preview captures; window mode uses live back-buffer.
        Result->SetNumberField(TEXT("max_size"), MaxSize);
        Result->SetStringField(TEXT("renderer"), TEXT("widgetRenderer"));
    }
    else
    {
        Result->SetStringField(TEXT("renderer"), TEXT("slateScreenshot"));
    }
    if (!OutPreviewName.IsEmpty())
    {
        Result->SetStringField(TEXT("previewName"), OutPreviewName);
    }
    Result->SetNumberField(TEXT("dpiScale"), OutDpiScale);
    Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
    Result->SetBoolField(TEXT("assetEditorWasAlreadyOpen"), ScopedDesigner.WasAlreadyOpen());
    Result->SetBoolField(TEXT("assetEditorClosed"), bAssetEditorClosed);
    Result->SetBoolField(TEXT("assetEditorCloseDeferred"),
        PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose(WidgetPath));
    if (bAlphaFactsMeasured)
    {
        // A widget's alpha 0 is real content -- FWidgetRenderer draws onto a target cleared
        // to FLinearColor::Transparent, so an uncovered region of a Designer preview is
        // genuinely transparent, not a back-buffer artefact. The capture stamps it opaque
        // anyway, because the unstamped PNG reads as blank in every alpha-compositing viewer
        // while its RGB is intact. `alphaZeroFraction` is what that decision owes the caller:
        // the amount of transparency the stamp destroyed, measured before it ran and
        // therefore unrecoverable from the file at `path`.
        Result->SetBoolField(TEXT("opaqueStamped"), bOpaqueStamped);
        if (bOpaqueStamped)
        {
            // Omitted rather than zeroed when no stamp ran: the fraction is counted by the
            // stamp pass, so without a stamp there is no measurement, and 0.0 would read as
            // "measured, nothing was transparent". Same shape as viewport.exposure's
            // adaptedMeasured/adapted pair.
            Result->SetNumberField(TEXT("alphaZeroFraction"), AlphaZeroFraction);
        }
    }
    if (bAnyOverrides)
    {
        // Three counts that distinguish:
        //   - showOnlyInputCount: how many widgets the caller named under `showOnly`
        //     (input-side count; the actual eye-flip count fan-outs to all non-keep widgets).
        //   - hiddenOverrideCount: how many preview widgets ended up with bHiddenInDesigner
        //     mutated for this capture (sum of showOnly fan-out + explicit `hide` entries).
        //   - visibilityOverrideCount: how many preview widgets had their runtime Visibility flipped.
        TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
        Applied->SetNumberField(TEXT("hiddenOverrideCount"), Guard.NumHiddenOverrides());
        Applied->SetNumberField(TEXT("visibilityOverrideCount"), Guard.NumVisibilityOverrides());
        Applied->SetNumberField(TEXT("showOnlyInputCount"), ShowOnly.Num());
        Result->SetObjectField(TEXT("overridesApplied"), Applied);
    }
    Ctx.SendSuccess(Result);
    return true;
}
