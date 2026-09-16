// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "Tests/Widget/WidgetTestFixtures.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "BlueprintModes/WidgetBlueprintApplicationModes.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/UI/WidgetDesignerCaptureInternal.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Rendering/SlateRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkitHost.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

using WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath;
using WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint;

namespace
{
    TSharedPtr<SWindow> FindWidgetDesignerScreenshotHostWindow(FWidgetBlueprintEditor* WidgetEditor)
    {
        if (!WidgetEditor)
        {
            return nullptr;
        }

        TSharedRef<IToolkitHost> ToolkitHost = WidgetEditor->GetToolkitHost();
        return FSlateApplication::Get().FindWidgetWindow(ToolkitHost->GetParentWidget());
    }

    void PumpSlateForWidgetDesignerScreenshot(FWidgetBlueprintEditor* WidgetEditor)
    {
        TSharedPtr<SWindow> HostWindow = FindWidgetDesignerScreenshotHostWindow(WidgetEditor);
        FSlateApplication& SlateApp = FSlateApplication::Get();
        SlateApp.PumpMessages();
        SlateApp.Tick(ESlateTickType::All);
        if (HostWindow.IsValid())
        {
            SlateApp.ForceRedrawWindow(HostWindow.ToSharedRef());
        }
        if (FSlateRenderer* Renderer = SlateApp.GetRenderer())
        {
            Renderer->FlushCommands();
        }
    }

    void PrepareWidgetDesignerScreenshotPreview(FWidgetBlueprintEditor* WidgetEditor)
    {
        if (!WidgetEditor)
        {
            return;
        }

        WidgetEditor->SetCurrentMode(FWidgetBlueprintApplicationModes::DesignerMode);
        if (TSharedPtr<FTabManager> TabManager = WidgetEditor->GetToolkitHost()->GetTabManager())
        {
            TabManager->TryInvokeTab(FTabId(FName(TEXT("SlatePreview"))));
        }
        WidgetEditor->InvalidatePreview(true);
        PumpSlateForWidgetDesignerScreenshot(WidgetEditor);
    }

    // The UMG Designer's preview canvas is sized from the WBP's CDO `DesignSizeMode` +
    // `DesignTimeSize` UPROPERTY pair. Setting `Custom` + an explicit size pins the
    // preview geometry to a known value so tests can assert exact pixel dimensions
    // without depending on monitor DPI or panel layout.
    void SetDesignerPreviewSizeOnCDO(UWidgetBlueprint* WBP, FVector2D Size)
    {
        if (!WBP || !WBP->GeneratedClass)
        {
            return;
        }
        UUserWidget* CDO = Cast<UUserWidget>(WBP->GeneratedClass->GetDefaultObject());
        if (!CDO)
        {
            return;
        }
        CDO->DesignSizeMode = EDesignPreviewSizeMode::Custom;
        CDO->DesignTimeSize = Size;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerRequiresWidgetPathTest,
    "PinWright.widget.screenshot_designer.RequiresWidgetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerRequiresWidgetPathTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"),
        MakeShared<FJsonObject>(), Capture);

    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("empty payload rejected"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerReadinessGuardTest,
    "PinWright.widget.screenshot_designer.ReadinessGuard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerReadinessGuardTest::RunTest(const FString& Parameters)
{
    using namespace WidgetDesignerCaptureInternal;

    FString Error;
    FDesignerCaptureReadinessState State;
    State.bEditorLoadingPackage = true;
    State.bEditorSelectionSetAvailable = true;
    TestFalse(TEXT("map/package loading blocks Designer Slate"),
        IsDesignerCaptureReady(State, Error));
    TestEqual(TEXT("map/package loading returns EDITOR_NOT_READY"),
        Error, FString(ErrorCodes::ERR_EDITOR_NOT_READY));

    State.bEditorLoadingPackage = false;
    State.bEditorSelectionSetAvailable = false;
    TestFalse(TEXT("missing editor selection set blocks Designer Slate"),
        IsDesignerCaptureReady(State, Error));
    TestEqual(TEXT("missing selection set returns EDITOR_NOT_READY"),
        Error, FString(ErrorCodes::ERR_EDITOR_NOT_READY));

    State.bEditorSelectionSetAvailable = true;
    TestTrue(TEXT("ready state permits Designer Slate"),
        IsDesignerCaptureReady(State, Error));
    TestTrue(TEXT("ready state clears the error"), Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerWindowCapturesAfterOpenAssetTest,
    "PinWright.widget.screenshot_designer.WindowCapturesAfterOpenAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerWindowCapturesAfterOpenAssetTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_DesignerScreenshot"));
    UWidgetBlueprint* WBP = MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
            }
        }
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("asset editor subsystem available"), AssetEditorSubsystem);
    if (!AssetEditorSubsystem)
    {
        return false;
    }

    TestTrue(TEXT("asset editor opened"),
        AssetEditorSubsystem->OpenEditorForAsset(WBP));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("target"), TEXT("window"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    TestTrue(TEXT("window screenshot succeeds"), Capture.bSuccess);
    TestTrue(TEXT("window screenshot result returned"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("screenshot path returned"),
        Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath) && ScreenshotPath.EndsWith(TEXT(".png")));
    TestTrue(TEXT("screenshot width returned"),
        Capture.Result->GetNumberField(TEXT("width")) > 0.0);
    TestTrue(TEXT("screenshot height returned"),
        Capture.Result->GetNumberField(TEXT("height")) > 0.0);
    TestEqual(TEXT("capture source is editor window"),
        Capture.Result->GetStringField(TEXT("captureSource")), FString(TEXT("editorWindow")));
    return true;
}

// Counterfactual: if ResolveDesignerViewWidget reverts to strict GetType()=="SDesignerView",
// the lookup misses TToolCompatibleMixin<SDesignerView> and the handler returns
// DESIGNER_SURFACE_NOT_FOUND. If the retry loop reverts to RefreshPreview-on-each-attempt,
// the preview UUserWidget is re-destroyed before SDesignerView::Tick can call TakeWidget
// and the handler returns PREVIEW_SLATE_NOT_FOUND. Either failure mode breaks success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerPreviewSelfOpensDesignerTest,
    "PinWright.widget.screenshot_designer.PreviewSelfOpensDesigner",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerPreviewSelfOpensDesignerTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_DesignerPreviewSelfOpen"));
    UWidgetBlueprint* WBP = MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }

    // Add a sized text-block child so the preview canvas has non-zero bounds — this
    // sidesteps PREVIEW_BOUNDS_NOT_FOUND once the cached Slate widget resolves.
    UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    TestNotNull(TEXT("root canvas exists"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }
    UTextBlock* PreviewLabel = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("PreviewLabel"));
    TestNotNull(TEXT("preview label constructed"), PreviewLabel);
    if (!PreviewLabel)
    {
        return false;
    }
    if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(PreviewLabel)))
    {
        CanvasSlot->SetSize(FVector2D(200.0, 100.0));
    }
    // Routes through the version-guarded fixture helper: no-op on UE 5.4
    // (no OnVariableAdded / GUID map), registers on 5.5+.
    WidgetTestFixtures::RegisterWidgetVariable(WBP, PreviewLabel->GetFName());
    FKismetEditorUtilities::CompileBlueprint(WBP);

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
            }
        }
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    // Cold-state precondition: close any open WidgetBlueprint editors so the handler
    // exercises the genuine cold-open path (not a warm tab reuse).
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("asset editor subsystem available"), AssetEditorSubsystem);
    if (!AssetEditorSubsystem)
    {
        return false;
    }
    {
        TArray<UObject*> OpenAssets = AssetEditorSubsystem->GetAllEditedAssets();
        for (UObject* Asset : OpenAssets)
        {
            if (UWidgetBlueprint* OpenWBP = Cast<UWidgetBlueprint>(Asset))
            {
                AssetEditorSubsystem->CloseAllEditorsForAsset(OpenWBP);
            }
        }
        // Bounded tick wait: the close path is async and we need it observed before asserting cold state.
        constexpr int32 MaxCloseTicks = 16;
        for (int32 Tick = 0; Tick < MaxCloseTicks; ++Tick)
        {
            if (AssetEditorSubsystem->FindEditorForAsset(WBP, /*bFocusIfOpen=*/false) == nullptr)
            {
                break;
            }
            FSlateApplication::Get().Tick(ESlateTickType::All);
        }
    }
    TestNull(TEXT("widget blueprint editor not open before handler call"),
        AssetEditorSubsystem->FindEditorForAsset(WBP, /*bFocusIfOpen=*/false));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("target"), TEXT("preview"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    TestTrue(TEXT("cold preview screenshot succeeds"), Capture.bSuccess);
    TestTrue(TEXT("preview screenshot result returned"), Capture.Result.IsValid());
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("screenshot path returned"),
        Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath) && ScreenshotPath.EndsWith(TEXT(".png")));
    TestTrue(TEXT("screenshot file exists"),
        !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
    TestTrue(TEXT("screenshot width returned"),
        Capture.Result->GetNumberField(TEXT("width")) > 0.0);
    TestTrue(TEXT("screenshot height returned"),
        Capture.Result->GetNumberField(TEXT("height")) > 0.0);
    TestEqual(TEXT("capture source is designer preview"),
        Capture.Result->GetStringField(TEXT("captureSource")), FString(TEXT("designerPreview")));

    // Post-condition: re-resolve the designer preview target and confirm the crop rect
    // populated. This catches a regression where the handler succeeds via a fallback path
    // but the SDesignerView lookup still doesn't work.
    FWidgetDesignerPreviewTarget PostTarget;
    FString PostError;
    const bool bPostResolved = FWidgetGeometryResolver::ResolveDesignerPreviewTarget(
        WBP, /*bFocusIfOpen=*/false, PostTarget, &PostError);
    TestTrue(TEXT("designer preview target resolves post-handler"), bPostResolved);
    TestTrue(TEXT("preview crop rect populated post-handler"), PostTarget.bHasPreviewCropRect);
    TestFalse(TEXT("preview crop rect is non-empty"), PostTarget.PreviewCropRect.IsEmpty());
    return true;
}

// Counterfactual: if the ON_SCOPE_EXIT revert in WidgetDesignerScreenshotHandler.cpp is removed, assertion (3) fails because PreviewChild0->bHiddenInDesigner remains true post-capture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerTransientOverridesRevertTest,
    "PinWright.widget.screenshot_designer.TransientOverridesRevert",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerTransientOverridesRevertTest::RunTest(const FString& Parameters)
{
    // Capture-and-revert: hide=["Child0"] flips bHiddenInDesigner on the preview's
    // Child0 for the screenshot, then ON_SCOPE_EXIT in the handler reverts it.
    // Asserts:
    //   1. Template-side bHiddenInDesigner unchanged (overrides only touch the preview).
    //   2. Preview-side Child0 resolvable through BPEditor->GetPreview()->GetWidgetFromName.
    //   3. Preview-side Child0 reverted to its original bHiddenInDesigner.
    //   4. Asset package dirty flag unchanged across the call.
    //   5. Result payload includes overridesApplied object.
    const FString WidgetPath = MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_DesignerScreenshotOverrides"));
    UWidgetBlueprint* WBP = MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }

    UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    TestNotNull(TEXT("root canvas exists"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }

    UTextBlock* TemplateChild0 = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("Child0"));
    TestNotNull(TEXT("Child0 constructed"), TemplateChild0);
    if (!TemplateChild0)
    {
        return false;
    }
    RootCanvas->AddChild(TemplateChild0);
    WidgetTestFixtures::RegisterWidgetVariable(WBP, TemplateChild0->GetFName());
    FKismetEditorUtilities::CompileBlueprint(WBP);

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
            }
        }
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("asset editor subsystem available"), AssetEditorSubsystem);
    if (!AssetEditorSubsystem)
    {
        return false;
    }

    TestTrue(TEXT("asset editor opened"),
        AssetEditorSubsystem->OpenEditorForAsset(WBP));

    // Pre-state snapshot.
    const bool bWasDirtyBefore = WBP->GetOutermost() && WBP->GetOutermost()->IsDirty();
    const bool bOldHidden = TemplateChild0->bHiddenInDesigner;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    // target="window" still routes through the override apply/revert path while sidestepping
    // preview-target resolution, which can be flaky on a freshly-compiled empty blueprint.
    Payload->SetStringField(TEXT("target"), TEXT("window"));

    TArray<TSharedPtr<FJsonValue>> HideArray;
    HideArray.Add(MakeShared<FJsonValueString>(TEXT("Child0")));
    Payload->SetArrayField(TEXT("hide"), HideArray);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    TestTrue(TEXT("screenshot succeeds with override"), Capture.bSuccess);
    if (Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath);
    }

    // 1. Template-side flag unchanged — overrides should never touch the asset's WidgetTree.
    TestEqual(TEXT("template hidden flag unchanged"),
        TemplateChild0->bHiddenInDesigner, bOldHidden);

    // 2-3. Preview-side flag reverted to its pre-call value.
    FWidgetBlueprintEditor* BPEditor = FWidgetGeometryResolver::FindWidgetBlueprintEditor(WBP, false);
    TestNotNull(TEXT("widget blueprint editor open"), BPEditor);
    UWidget* PreviewChild0 = nullptr;
    if (BPEditor)
    {
        if (UUserWidget* Preview = BPEditor->GetPreview())
        {
            PreviewChild0 = Preview->GetWidgetFromName(TEXT("Child0"));
        }
    }
    TestNotNull(TEXT("preview child0 resolvable"), PreviewChild0);
    if (PreviewChild0)
    {
        TestEqual(TEXT("preview hidden flag reverted"),
            PreviewChild0->bHiddenInDesigner, bOldHidden);
    }

    // 4. Asset package dirty flag unchanged across the call.
    const bool bDirtyAfter = WBP->GetOutermost() && WBP->GetOutermost()->IsDirty();
    TestEqual(TEXT("asset dirty unchanged"), bDirtyAfter, bWasDirtyBefore);

    // 5. The handler reports overridesApplied counts when overrides are in play.
    TestTrue(TEXT("overridesApplied returned"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("overridesApplied")));

    return true;
}

// Counterfactual: if WidgetDesignerScreenshotHandler.cpp captures the whole SDesignerView
// instead of passing DesignerTarget.PreviewCropRect to TakeScreenshot, the capture matches
// the designer-view bounds and the smaller-than-anchor assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetScreenshotDesignerPreviewMatchesCanvasBoundsTest,
    "PinWright.widget.screenshot_designer.PreviewMatchesCanvasBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerPreviewMatchesCanvasBoundsTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetDesignerScreenshotAssetPath(TEXT("WBP_DesignerPreviewChrome"));
    UWidgetBlueprint* WBP = MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }

    // Pin the designer preview to a deterministic 400x200 design-time size (chosen so
    // even at 2.0× DPI the absolute preview stays at 800x400 — well under the
    // SDesignerView anchor that observed runners measure at ~1173x1173). Without this
    // override the preview canvas inherits whatever default the SDesignerView measures,
    // which on high-DPI/large-monitor hosts overflows the anchor and makes the
    // capture<anchor assertion impossible to satisfy alongside capture==preview.
    constexpr double PinnedDesignWidth = 400.0;
    constexpr double PinnedDesignHeight = 200.0;
    FKismetEditorUtilities::CompileBlueprint(WBP);
    SetDesignerPreviewSizeOnCDO(WBP, FVector2D(PinnedDesignWidth, PinnedDesignHeight));

    FString ScreenshotPath;
    ON_SCOPE_EXIT
    {
        if (!ScreenshotPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ScreenshotPath);
        }
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
            }
        }
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("asset editor subsystem available"), AssetEditorSubsystem);
    if (!AssetEditorSubsystem)
    {
        return false;
    }

    TestTrue(TEXT("asset editor opened"),
        AssetEditorSubsystem->OpenEditorForAsset(WBP));

    FWidgetBlueprintEditor* BPEditor = FWidgetGeometryResolver::FindWidgetBlueprintEditor(WBP, true);
    TestNotNull(TEXT("widget blueprint editor open"), BPEditor);
    if (!BPEditor)
    {
        return false;
    }
    PrepareWidgetDesignerScreenshotPreview(BPEditor);

    // Resolve the designer-view anchor and preview crop BEFORE invoking the handler. The
    // resolver uses SDesignerView::GetWidgetGeometry so the expected bounds are the root
    // preview/canvas, not the larger designer surface with rulers and controls.
    FWidgetDesignerPreviewTarget DesignerTarget;
    FString ResolveError;
    bool bResolved = false;
    constexpr int32 MaxResolveAttempts = 6;
    for (int32 Attempt = 0; Attempt < MaxResolveAttempts; ++Attempt)
    {
        bResolved = FWidgetGeometryResolver::ResolveDesignerPreviewTarget(
            WBP, true, DesignerTarget, &ResolveError);
        if (bResolved)
        {
            break;
        }
        PumpSlateForWidgetDesignerScreenshot(BPEditor);
    }
    TestTrue(TEXT("designer preview target resolved"), bResolved);
    if (!bResolved)
    {
        return false;
    }

    TestNotNull(TEXT("designer view anchor resolved"), DesignerTarget.DesignerViewSlate.Get());
    if (!DesignerTarget.DesignerViewSlate.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("preview crop rect resolved"), DesignerTarget.bHasPreviewCropRect);
    if (!DesignerTarget.bHasPreviewCropRect)
    {
        return false;
    }

    FVector2D AnchorSize = FVector2D(DesignerTarget.DesignerViewGeometry.GetAbsoluteSize());
    FVector2D PreviewSize = FVector2D(DesignerTarget.PreviewGeometry.GetAbsoluteSize());
    if (AnchorSize.X <= 0.0 || AnchorSize.Y <= 0.0 || PreviewSize.X <= 0.0 || PreviewSize.Y <= 0.0)
    {
        PumpSlateForWidgetDesignerScreenshot(BPEditor);
        const bool bResolvedAfterPump = FWidgetGeometryResolver::ResolveDesignerPreviewTarget(WBP, true, DesignerTarget, &ResolveError);
        TestTrue(TEXT("designer target still has preview crop after Slate pump"), bResolvedAfterPump);
        if (!bResolvedAfterPump)
        {
            return false;
        }
        AnchorSize = FVector2D(DesignerTarget.DesignerViewGeometry.GetAbsoluteSize());
        PreviewSize = FVector2D(DesignerTarget.PreviewGeometry.GetAbsoluteSize());
    }
    TestTrue(
        FString::Printf(TEXT("designer view and preview have nonzero bounds (anchor=%.0fx%.0f, preview=%.0fx%.0f)"),
            AnchorSize.X, AnchorSize.Y, PreviewSize.X, PreviewSize.Y),
        AnchorSize.X > 0.0 && AnchorSize.Y > 0.0 && PreviewSize.X > 0.0 && PreviewSize.Y > 0.0);
    if (AnchorSize.X <= 0.0 || AnchorSize.Y <= 0.0 || PreviewSize.X <= 0.0 || PreviewSize.Y <= 0.0)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("target"), TEXT("preview"));
    // Set max_size = the *resolved* PreviewSize long axis (in absolute/DPI-scaled px)
    // so the handler's longer-axis cap math (Factor = MaxSize / max(PreviewSize.X,
    // PreviewSize.Y)) yields Factor=1.0 and PreviewDrawSize equals PreviewSize
    // exactly. Using the design-time constant directly would break under non-1.0 DPI
    // because absolute PreviewSize = DesignTimeSize * DpiScale. Lifting the cap
    // entirely (e.g. max_size=16384) instead super-samples the preview and breaks
    // the direct capture==preview equality below.
    const int32 MaxSize = FMath::Max(1, FMath::RoundToInt(FMath::Max(PreviewSize.X, PreviewSize.Y)));
    Payload->SetNumberField(TEXT("max_size"), MaxSize);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.screenshot_designer handler found"),
        InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
    TestTrue(TEXT("preview screenshot succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath);

    const double CaptureWidth = Capture.Result->GetNumberField(TEXT("width"));
    const double CaptureHeight = Capture.Result->GetNumberField(TEXT("height"));

    // ±4px tolerance for rounding / DPI scaling on the size comparisons.
    constexpr double Tolerance = 4.0;

    const bool bMatchesPreviewBounds =
        FMath::Abs(CaptureWidth - PreviewSize.X) <= Tolerance &&
        FMath::Abs(CaptureHeight - PreviewSize.Y) <= Tolerance;
    TestTrue(
        FString::Printf(TEXT("capture matches preview root/canvas bounds (capture=%.0fx%.0f, preview=%.0fx%.0f)"),
            CaptureWidth, CaptureHeight, PreviewSize.X, PreviewSize.Y),
        bMatchesPreviewBounds);

    const bool bSmallerThanDesignerView =
        CaptureWidth < AnchorSize.X - Tolerance ||
        CaptureHeight < AnchorSize.Y - Tolerance;
    TestTrue(
        FString::Printf(TEXT("capture is smaller than designer view anchor (capture=%.0fx%.0f, anchor=%.0fx%.0f)"),
            CaptureWidth, CaptureHeight, AnchorSize.X, AnchorSize.Y),
        bSmallerThanDesignerView);

    // Sanity: capture should never exceed the host window's client area.
    if (DesignerTarget.HostWindow.IsValid())
    {
        const FVector2D HostClient = FVector2D(DesignerTarget.HostWindow->GetClientSizeInScreen());
        TestTrue(
            FString::Printf(TEXT("capture fits inside host client area (capture=%.0fx%.0f, host=%.0fx%.0f)"),
                CaptureWidth, CaptureHeight, HostClient.X, HostClient.Y),
            CaptureWidth <= HostClient.X + Tolerance && CaptureHeight <= HostClient.Y + Tolerance);
    }

    return true;
}

namespace
{
    // The UMG Designer's preview canvas is sized from the WBP's CDO `DesignSizeMode` +
    // `DesignTimeSize` UPROPERTY pair. Setting `Custom` + an explicit size pins the
    // preview geometry to a known value so the test can assert exact aspect-ratio math
    // on the captured PNG without depending on monitor DPI or panel layout.
    void SetDesignerPreviewSize(UWidgetBlueprint* WBP, FVector2D Size)
    {
        if (!WBP || !WBP->GeneratedClass)
        {
            return;
        }
        UUserWidget* CDO = Cast<UUserWidget>(WBP->GeneratedClass->GetDefaultObject());
        if (!CDO)
        {
            return;
        }
        CDO->DesignSizeMode = EDesignPreviewSizeMode::Custom;
        CDO->DesignTimeSize = Size;
    }
}

// Counterfactual: if the `max_size` plumbing into `DrawSize` is reverted (restoring
// `FSlateApplication::TakeScreenshot` on the preview path), the second portrait capture's
// `width`/`height` will not equal `(≈512, 1024)` — they will fall back to whatever the
// live Designer canvas measures on screen, which is monitor-DPI-dependent and never
// exactly 1024 on the longer axis. Both portrait and landscape assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FWidgetScreenshotDesignerMaxSizePreviewTest,
    "PinWright.widget.screenshot_designer.MaxSizeCapsLongestAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetScreenshotDesignerMaxSizePreviewTest::RunTest(const FString& Parameters)
{
    // Three-pass test: landscape (800x400, max_size=1024 → 1024x512), portrait (400x800,
    // max_size=1024 → 512x1024), and default (omit max_size, expect 1024 default applied).
    // ±4px tolerance on the shorter axis to absorb FMath::RoundToInt rounding.
    // static: MSVC 14.38 (UE 5.3 toolchain) rejects implicit lambda use of non-captured constexpr locals.
    static constexpr double ShortAxisTolerance = 4.0;

    auto RunOneCase = [this](const TCHAR* CasePrefix, FVector2D DesignSize, int32* MaxSizeOpt,
        int32 ExpectedLong, int32 ExpectedShort, bool bExpectLandscape) -> bool
    {
        const FString WidgetPath = MakeWidgetDesignerScreenshotAssetPath(CasePrefix);
        UWidgetBlueprint* WBP = MakeWidgetDesignerScreenshotBlueprint(WidgetPath);
        TestNotNull(TEXT("widget blueprint allocated"), WBP);
        if (!WBP)
        {
            return false;
        }

        // Add a sized child so the preview canvas has non-zero rendered content; without
        // a child the offscreen FWidgetRenderer pass can produce a fully transparent target.
        UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
        if (RootCanvas)
        {
            UTextBlock* Label = WBP->WidgetTree->ConstructWidget<UTextBlock>(
                UTextBlock::StaticClass(), TEXT("PreviewLabel"));
            if (Label)
            {
                if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(Label)))
                {
                    CanvasSlot->SetSize(FVector2D(50.0, 25.0));
                }
                WidgetTestFixtures::RegisterWidgetVariable(WBP, Label->GetFName());
            }
        }
        FKismetEditorUtilities::CompileBlueprint(WBP);

        // Apply CDO design-size override after compile so the freshly-generated CDO
        // carries the override (CompileBlueprint regenerates the GeneratedClass).
        SetDesignerPreviewSize(WBP, DesignSize);

        FString ScreenshotPath;
        ON_SCOPE_EXIT
        {
            if (!ScreenshotPath.IsEmpty())
            {
                IFileManager::Get().Delete(*ScreenshotPath);
            }
            if (GEditor)
            {
                if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
                {
                    AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
                }
            }
            if (UPackage* Package = WBP->GetOutermost())
            {
                Package->SetDirtyFlag(false);
            }
            CleanupTestAsset(WidgetPath);
        };

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        Payload->SetStringField(TEXT("target"), TEXT("preview"));
        if (MaxSizeOpt)
        {
            Payload->SetNumberField(TEXT("max_size"), *MaxSizeOpt);
        }

        FTestResponseCapture Capture;
        TestTrue(TEXT("widget.screenshot_designer handler found"),
            InvokeHandlerWithCapture(TEXT("widget.screenshot_designer"), Payload, Capture));
        TestTrue(FString::Printf(TEXT("[%s] preview screenshot succeeds"), CasePrefix), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        Capture.Result->TryGetStringField(TEXT("path"), ScreenshotPath);

        const double CaptureWidth = Capture.Result->GetNumberField(TEXT("width"));
        const double CaptureHeight = Capture.Result->GetNumberField(TEXT("height"));

        if (bExpectLandscape)
        {
            TestEqual(FString::Printf(TEXT("[%s] landscape width caps at max_size"), CasePrefix),
                static_cast<int32>(CaptureWidth), ExpectedLong);
            TestTrue(
                FString::Printf(TEXT("[%s] landscape height ~= %d (got %.0f)"),
                    CasePrefix, ExpectedShort, CaptureHeight),
                FMath::Abs(CaptureHeight - ExpectedShort) <= ShortAxisTolerance);
        }
        else
        {
            TestEqual(FString::Printf(TEXT("[%s] portrait height caps at max_size"), CasePrefix),
                static_cast<int32>(CaptureHeight), ExpectedLong);
            TestTrue(
                FString::Printf(TEXT("[%s] portrait width ~= %d (got %.0f)"),
                    CasePrefix, ExpectedShort, CaptureWidth),
                FMath::Abs(CaptureWidth - ExpectedShort) <= ShortAxisTolerance);
        }

        TestEqual(FString::Printf(TEXT("[%s] max_size echoed in response"), CasePrefix),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("max_size"))),
            MaxSizeOpt ? *MaxSizeOpt : 1024);
        TestEqual(FString::Printf(TEXT("[%s] renderer is widgetRenderer"), CasePrefix),
            Capture.Result->GetStringField(TEXT("renderer")), FString(TEXT("widgetRenderer")));
        return true;
    };

    // Case 1: landscape 800x400, max_size=1024 → 1024x512.
    int32 Max1024 = 1024;
    RunOneCase(TEXT("WBP_DesignerMaxSizeLandscape"), FVector2D(800.0, 400.0),
        &Max1024, /*ExpectedLong=*/1024, /*ExpectedShort=*/512, /*bExpectLandscape=*/true);

    // Case 2: portrait 400x800, max_size=1024 → 512x1024. Proves the cap targets the
    // longer axis (height here), not always-width.
    RunOneCase(TEXT("WBP_DesignerMaxSizePortrait"), FVector2D(400.0, 800.0),
        &Max1024, /*ExpectedLong=*/1024, /*ExpectedShort=*/512, /*bExpectLandscape=*/false);

    // Case 3: omit max_size; expect default 1024 applied.
    RunOneCase(TEXT("WBP_DesignerMaxSizeDefault"), FVector2D(800.0, 400.0),
        /*MaxSizeOpt=*/nullptr, /*ExpectedLong=*/1024, /*ExpectedShort=*/512, /*bExpectLandscape=*/true);

    return true;
}
