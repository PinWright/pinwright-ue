// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "WidgetGeometryResolver.h"

#include "Compat/EngineVersionCompat.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

// Slate
#include "Application/SlateApplicationBase.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWidget.h"
#include "Widgets/SVirtualWindow.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/ArrangedWidget.h"
#include "Rendering/SlateLayoutTransform.h"

// UMG
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Slate/WidgetTransform.h"

// UMG Editor
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkitHost.h"
#include "Designer/SDesignerView.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"

// ---------------------------------------------------------------------------
// Internal forward declarations
// ---------------------------------------------------------------------------
static FWidgetGeometryResult ResolveViaDesigner(const FWidgetGeometryRequest& Request);
static FWidgetGeometryResult ResolveViaLive(const FWidgetGeometryRequest& Request);
static FWidgetGeometryResult ResolveViaOffscreen(const FWidgetGeometryRequest& Request);

static void PopulateSlateGeometryMap(
    TSharedRef<SWidget> RootSlate,
    const FGeometry& RootGeometry,
    TMap<TSharedPtr<const SWidget>, FGeometry>& Out);

static void FillResolvedGeometry(
    FResolvedGeometry& OutGeo,
    const FGeometry& G,
    UWidget* W);

static void FillResolvedGeometryFromMap(
    FResolvedGeometry& OutGeo,
    const FGeometry& G,
    UWidget* W,
    TSharedPtr<SWidget> Underlying);

FWidgetBlueprintEditor* FWidgetGeometryResolver::FindWidgetBlueprintEditor(
    UWidgetBlueprint* Blueprint,
    bool bFocusIfOpen)
{
    UAssetEditorSubsystem* AssetEditorSS = GEditor
        ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
        : nullptr;
    if (!AssetEditorSS || !Blueprint)
    {
        return nullptr;
    }

    IAssetEditorInstance* EditorInstance = AssetEditorSS->FindEditorForAsset(Blueprint, bFocusIfOpen);
    if (!EditorInstance || EditorInstance->GetEditorName() != FName(TEXT("WidgetBlueprintEditor")))
    {
        return nullptr;
    }

    return static_cast<FWidgetBlueprintEditor*>(EditorInstance);
}

// Resolve the SDesignerView Slate widget that owns the WBP designer's preview geometry cache.
// Primary: tab lookup via FDesignerTabSummoner::TabID ("SlatePreview"). Fallback: walk parents
// of PreviewSlate looking for an SDesignerView by type name.
// Returns null if neither path resolves; callers decide whether that's fatal.
static TSharedPtr<SDesignerView> ResolveDesignerViewWidget(
    FWidgetBlueprintEditor* WidgetEditor,
    TSharedPtr<SWidget> PreviewSlate)
{
    if (!WidgetEditor)
    {
        return nullptr;
    }

    // Exact-match the type name against the two known Slate types that derive from
    // SDesignerView: the bare class and the UE 5.6 DesignerTabSummoner wrapper
    // TToolCompatibleMixin<SDesignerView>. A substring `Contains` would also match
    // unrelated types like SDesignerViewportPane, where the static_cast would be UB.
    // If a future wrapper appears, add it here explicitly.
    auto AsDesignerView = [&](TSharedPtr<SWidget> Widget) -> TSharedPtr<SDesignerView>
    {
        if (!Widget.IsValid())
        {
            return nullptr;
        }
        const FString TypeName = Widget->GetType().ToString();
        if (TypeName.Equals(TEXT("SDesignerView")) ||
            TypeName.Equals(TEXT("TToolCompatibleMixin<SDesignerView>")))
        {
            return StaticCastSharedPtr<SDesignerView>(Widget);
        }
        return nullptr;
    };

    if (TSharedPtr<FTabManager> TabManager = WidgetEditor->GetToolkitHost()->GetTabManager())
    {
        if (TSharedPtr<SDockTab> DesignerTab = TabManager->FindExistingLiveTab(FTabId(FName(TEXT("SlatePreview")))))
        {
            if (TSharedPtr<SDesignerView> DesignerView = AsDesignerView(DesignerTab->GetContent()))
            {
                return DesignerView;
            }
        }
    }

    // Fallback parent-walk; depth-capped so cyclic chains can't spin forever.
    constexpr int32 MaxParentWalkDepth = 32;
    TSharedPtr<SWidget> Cursor = PreviewSlate;
    for (int32 i = 0; i < MaxParentWalkDepth && Cursor.IsValid(); ++i)
    {
        if (TSharedPtr<SDesignerView> DesignerView = AsDesignerView(Cursor))
        {
            return DesignerView;
        }
        Cursor = Cursor->GetParentWidget();
    }
    return nullptr;
}

static bool BuildPreviewCropRect(
    const FGeometry& AnchorGeometry,
    const FGeometry& PreviewGeometry,
    FIntRect& OutCropRect)
{
    const FVector2D AnchorAbsolutePosition = FVector2D(AnchorGeometry.GetAbsolutePosition());
    const FVector2D PreviewAbsolutePosition = FVector2D(PreviewGeometry.GetAbsolutePosition());
    const FVector2D PreviewAbsoluteSize = FVector2D(PreviewGeometry.GetAbsoluteSize());

    const FVector2D RelativeMin = PreviewAbsolutePosition - AnchorAbsolutePosition;
    const FVector2D RelativeMax = RelativeMin + PreviewAbsoluteSize;

    OutCropRect = FIntRect(
        FMath::FloorToInt(RelativeMin.X),
        FMath::FloorToInt(RelativeMin.Y),
        FMath::CeilToInt(RelativeMax.X),
        FMath::CeilToInt(RelativeMax.Y));
    return !OutCropRect.IsEmpty();
}

bool FWidgetGeometryResolver::ResolveDesignerPreviewTarget(
    UWidgetBlueprint* Blueprint,
    bool bFocusIfOpen,
    FWidgetDesignerPreviewTarget& OutTarget,
    FString* OutError,
    bool bResolveDesignerSurface)
{
    OutTarget = FWidgetDesignerPreviewTarget{};
    if (OutError)
    {
        OutError->Reset();
    }

    FWidgetBlueprintEditor* BPEditor = FindWidgetBlueprintEditor(Blueprint, bFocusIfOpen);
    if (!BPEditor)
    {
        if (OutError)
        {
            *OutError = TEXT("EDITOR_NOT_FOUND");
        }
        return false;
    }

    UUserWidget* Preview = BPEditor->GetPreview();
    if (!Preview)
    {
        if (OutError)
        {
            *OutError = TEXT("PREVIEW_NOT_FOUND");
        }
        return false;
    }

    TSharedPtr<SWidget> PreviewSlate = Preview->GetCachedWidget();
    if (!PreviewSlate.IsValid())
    {
        if (OutError)
        {
            *OutError = TEXT("PREVIEW_SLATE_NOT_FOUND");
        }
        return false;
    }

    TSharedPtr<SWindow> HostWindow = FSlateApplication::Get().FindWidgetWindow(PreviewSlate.ToSharedRef());

    OutTarget.Editor = BPEditor;
    OutTarget.Preview = Preview;
    OutTarget.PreviewSlate = PreviewSlate;
    if (bResolveDesignerSurface)
    {
        // PREVIEW_SLATE_NOT_FOUND (above) means the cached UUserWidget Slate is null —
        // SDesignerView::Tick has not yet run TakeWidget. DESIGNER_SURFACE_NOT_FOUND
        // means the cached widget exists but the SDesignerView host couldn't be located,
        // which points at a wrapper-type mismatch instead of the tick race.
        TSharedPtr<SDesignerView> DesignerView = ResolveDesignerViewWidget(BPEditor, PreviewSlate);
        if (!DesignerView.IsValid())
        {
            if (OutError)
            {
                *OutError = TEXT("DESIGNER_SURFACE_NOT_FOUND");
            }
            return false;
        }

        FGeometry PreviewGeometry;
        if (!DesignerView->GetWidgetGeometry(Preview, PreviewGeometry))
        {
            if (OutError)
            {
                *OutError = TEXT("PREVIEW_BOUNDS_NOT_FOUND");
            }
            return false;
        }

        const FGeometry DesignerViewGeometry = DesignerView->GetTickSpaceGeometry();
        FIntRect PreviewCropRect;
        if (!BuildPreviewCropRect(DesignerViewGeometry, PreviewGeometry, PreviewCropRect))
        {
            if (OutError)
            {
                *OutError = TEXT("PREVIEW_BOUNDS_NOT_FOUND");
            }
            return false;
        }

        OutTarget.DesignerViewSlate = StaticCastSharedPtr<SWidget>(DesignerView);
        OutTarget.DesignerViewGeometry = DesignerViewGeometry;
        OutTarget.PreviewGeometry = PreviewGeometry;
        OutTarget.PreviewCropRect = PreviewCropRect;
        OutTarget.bHasPreviewCropRect = true;
    }
    OutTarget.HostWindow = HostWindow;
    OutTarget.RootGeometry = Preview->GetCachedGeometry();
    OutTarget.DpiScale = HostWindow.IsValid() ? HostWindow->GetDPIScaleFactor() : 1.0f;
    return true;
}

// Path-based detection avoids a hard link against the UIExtension module: we resolve
// the class once via FindObject (returns nullptr if the module isn't loaded) and then
// compare UClass pointers per widget.
// Walk via ForEachWidgetAndDescendants so we cross nested UUserWidget tree boundaries —
// the offscreen TakeWidget cascade rebuilds those subtrees too, so detection has to match.
bool FWidgetGeometryResolver::ContainsExtensionPointWidget(UWidgetTree* Tree)
{
    if (!Tree)
    {
        return false;
    }

    static const UClass* ExtPointClass = FindObject<UClass>(
        nullptr, TEXT("/Script/UIExtension.UIExtensionPointWidget"));
    if (!ExtPointClass)
    {
        return false;
    }

    bool bFound = false;
    Tree->ForEachWidgetAndDescendants([&](UWidget* W)
    {
        if (!bFound && W && W->GetClass() == ExtPointClass)
        {
            bFound = true;
        }
    });
    return bFound;
}

// ---------------------------------------------------------------------------
// FillResolvedGeometry
// Populate a FResolvedGeometry from a FGeometry + UWidget.
// ---------------------------------------------------------------------------
static void FillResolvedGeometry(FResolvedGeometry& OutGeo, const FGeometry& G, UWidget* W)
{
    if (!W)
    {
        OutGeo.Status = FResolvedGeometry::EStatus::Error;
        OutGeo.Reason = TEXT("NULL_WIDGET");
        return;
    }

    TSharedPtr<SWidget> Underlying = W->GetCachedWidget();
    if (!Underlying.IsValid())
    {
        OutGeo.Status = FResolvedGeometry::EStatus::Error;
        OutGeo.Reason = TEXT("NOT_PAINTED");
        return;
    }

    OutGeo.AbsolutePos  = FVector2D(G.GetAbsolutePosition());
    OutGeo.AbsoluteSize = FVector2D(G.GetAbsoluteSize());
    OutGeo.LocalSize    = FVector2D(G.GetLocalSize());
    OutGeo.DesiredSize  = FVector2D(Underlying->GetDesiredSize());
    OutGeo.EffectiveVisibility = UWidget::ConvertSerializedVisibilityToRuntime(W->GetVisibility());

    // Render transform: use FWidgetTransform::IsIdentity() (compares == with default-constructed value).
    const FWidgetTransform WT = W->GetRenderTransform();
    OutGeo.bHasRenderTransform  = !WT.IsIdentity();
    OutGeo.RenderTransformValue = WT;
    OutGeo.Status               = FResolvedGeometry::EStatus::Ok;
}

// Offscreen-tier variant: caller passes in the already-looked-up Underlying slate widget,
// so we don't call UWidget::GetCachedWidget() a second time after the SlateGeometryMap lookup.
static void FillResolvedGeometryFromMap(
    FResolvedGeometry& OutGeo,
    const FGeometry& G,
    UWidget* W,
    TSharedPtr<SWidget> Underlying)
{
    if (!W)
    {
        OutGeo.Status = FResolvedGeometry::EStatus::Error;
        OutGeo.Reason = TEXT("NULL_WIDGET");
        return;
    }

    OutGeo.AbsolutePos  = FVector2D(G.GetAbsolutePosition());
    OutGeo.AbsoluteSize = FVector2D(G.GetAbsoluteSize());
    OutGeo.LocalSize    = FVector2D(G.GetLocalSize());
    OutGeo.DesiredSize  = Underlying.IsValid()
        ? FVector2D(Underlying->GetDesiredSize())
        : FVector2D::ZeroVector;

    OutGeo.EffectiveVisibility = UWidget::ConvertSerializedVisibilityToRuntime(W->GetVisibility());

    const FWidgetTransform WT = W->GetRenderTransform();
    OutGeo.bHasRenderTransform  = !WT.IsIdentity();
    OutGeo.RenderTransformValue = WT;
    OutGeo.Status               = FResolvedGeometry::EStatus::Ok;
}

// ---------------------------------------------------------------------------
// PopulateSlateGeometryMap
// Mirrors SDesignerView::PopulateWidgetGeometryCache_Loop.
// Walks arranged children recursively and records each SWidget's FGeometry.
// RootGeometry is the already-allotted geometry for RootSlate (caller provides it).
// ---------------------------------------------------------------------------
static void PopulateSlateGeometryMap(
    TSharedRef<SWidget> RootSlate,
    const FGeometry& RootGeometry,
    TMap<TSharedPtr<const SWidget>, FGeometry>& Out)
{
    // Iterative DFS mirror of PopulateWidgetGeometryCache_Loop.
    // We use a stack to avoid deep recursion on large widget trees.
    struct FEntry
    {
        TSharedRef<SWidget> Widget;
        FGeometry Geometry;
    };

    TArray<FEntry> Stack;
    Stack.Push({ RootSlate, RootGeometry });

    while (Stack.Num() > 0)
    {
        FEntry Current = Stack.Pop(EAllowShrinking::No);

        // Use EVisibility::All so collapsed/hidden children are still included
        // (we want geometry for everything, visibility is reported separately).
        FArrangedChildren ArrangedChildren(EVisibility::All);
        Current.Widget->ArrangeChildren(Current.Geometry, ArrangedChildren);

        for (int32 i = 0; i < ArrangedChildren.Num(); ++i)
        {
            const FArrangedWidget& Child = ArrangedChildren[i];
            TSharedRef<SWidget> ChildWidget = Child.Widget;
            Out.Add(ChildWidget, Child.Geometry);
            Stack.Push({ ChildWidget, Child.Geometry });
        }
    }
}

// ---------------------------------------------------------------------------
// Tier A — Designer preview
// ---------------------------------------------------------------------------
static FWidgetGeometryResult ResolveViaDesigner(const FWidgetGeometryRequest& Request)
{
    FWidgetGeometryResult Result;
    Result.ServedBy = EWidgetGeometrySource::Designer;

    UWidgetBlueprint* Blueprint = Request.Blueprint;
    if (!Blueprint)
    {
        Result.TopLevelError = TEXT("NULL_BLUEPRINT");
        return Result;
    }

    FWidgetDesignerPreviewTarget DesignerTarget;
    if (!FWidgetGeometryResolver::ResolveDesignerPreviewTarget(Blueprint, false, DesignerTarget))
    {
        return Result;
    }

    // Walk the preview tree and fill geometry from cached geometry.
    UUserWidget* Preview = DesignerTarget.Preview;
    const FGeometry RootGeo = DesignerTarget.RootGeometry;

    // Root entry keyed by the preview UUserWidget* pointer.
    {
        FResolvedGeometry RootResolved;
        FillResolvedGeometry(RootResolved, RootGeo, Preview);
        Result.ByWidget.Add(FObjectKey(Preview), RootResolved);
    }

    Preview->WidgetTree->ForEachWidget([&](UWidget* W)
    {
        if (!W)
        {
            return;
        }
        FResolvedGeometry Geo;
        FGeometry G = W->GetCachedGeometry();
        FillResolvedGeometry(Geo, G, W);
        Result.ByWidget.Add(FObjectKey(W), Geo);
    });

    Result.DpiScale         = DesignerTarget.DpiScale;
    Result.ViewportSizeUsed = FVector2D(RootGeo.GetAbsoluteSize());
    Result.LiveRoot         = Preview;

    return Result;
}

// ---------------------------------------------------------------------------
// Tier B — Live on-screen instance
// ---------------------------------------------------------------------------
static FWidgetGeometryResult ResolveViaLive(const FWidgetGeometryRequest& Request)
{
    FWidgetGeometryResult Result;
    Result.ServedBy = EWidgetGeometrySource::Live;

    UWidgetBlueprint* Blueprint = Request.Blueprint;
    if (!Blueprint)
    {
        Result.TopLevelError = TEXT("NULL_BLUEPRINT");
        return Result;
    }

    UClass* GeneratedClass = Blueprint->GeneratedClass;
    if (!GeneratedClass)
    {
        Result.TopLevelError = TEXT("BLUEPRINT_NOT_COMPILED");
        return Result;
    }

    // Collect live instances from both editor and game worlds (mirrors UiHandler.cpp L341-345).
    TArray<UUserWidget*> AllWidgets;

    auto CollectFromWorld = [&](UWorld* World)
    {
        if (!World)
        {
            return;
        }
        TArray<UUserWidget*> Found;
        UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, Found, GeneratedClass, /*TopLevelOnly=*/false);
        AllWidgets.Append(Found);
    };

    CollectFromWorld(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr);
    CollectFromWorld((GEngine && GEngine->GameViewport) ? GEngine->GameViewport->GetWorld() : nullptr);

    // Filter to only those that are visible and have valid geometry.
    TArray<UUserWidget*> Candidates;
    for (UUserWidget* W : AllWidgets)
    {
        if (!W)
        {
            continue;
        }
        if (!W->IsInViewport())
        {
            continue;
        }
        TSharedPtr<SWidget> Slate = W->GetCachedWidget();
        if (!Slate.IsValid())
        {
            continue;
        }
        const FVector2D LocalSz = FVector2D(W->GetCachedGeometry().GetLocalSize());
        if (LocalSz.SizeSquared() <= 0.0)
        {
            continue;
        }
        Candidates.Add(W);
    }

    if (Candidates.Num() == 0)
    {
        // No live instance — fall through.
        return Result;
    }

    // Disambiguate by LiveInstanceName if provided.
    UUserWidget* Chosen = nullptr;
    if (!Request.LiveInstanceName.IsEmpty())
    {
        for (UUserWidget* C : Candidates)
        {
            if (C->GetName().Contains(Request.LiveInstanceName))
            {
                Chosen = C;
                break;
            }
        }
    }
    else if (Candidates.Num() == 1)
    {
        Chosen = Candidates[0];
    }

    if (!Chosen)
    {
        // Ambiguous — report it.
        Result.bAmbiguous = true;
        for (UUserWidget* C : Candidates)
        {
            Result.AmbiguousCandidates.Add(C);
        }
        return Result;
    }

    // Measure the chosen instance.
    float DpiScale = 1.0f;
    TSharedPtr<SWidget> ChosenSlate = Chosen->GetCachedWidget();
    if (ChosenSlate.IsValid())
    {
        TSharedPtr<SWindow> HostWindow = FSlateApplication::Get().FindWidgetWindow(ChosenSlate.ToSharedRef());
        if (HostWindow.IsValid())
        {
            DpiScale = HostWindow->GetDPIScaleFactor();
        }
    }

    const FGeometry RootGeo = Chosen->GetCachedGeometry();

    // Root entry.
    {
        FResolvedGeometry RootResolved;
        FillResolvedGeometry(RootResolved, RootGeo, Chosen);
        Result.ByWidget.Add(FObjectKey(Chosen), RootResolved);
    }

    Chosen->WidgetTree->ForEachWidget([&](UWidget* W)
    {
        if (!W)
        {
            return;
        }
        FResolvedGeometry Geo;
        FGeometry G = W->GetCachedGeometry();
        FillResolvedGeometry(Geo, G, W);
        Result.ByWidget.Add(FObjectKey(W), Geo);
    });

    Result.DpiScale         = DpiScale;
    Result.ViewportSizeUsed = FVector2D(RootGeo.GetAbsoluteSize());
    Result.LiveRoot         = Chosen;

    return Result;
}

// ---------------------------------------------------------------------------
// Tier C — Off-screen virtual window
// ---------------------------------------------------------------------------
static FWidgetGeometryResult ResolveViaOffscreen(const FWidgetGeometryRequest& Request)
{
    FWidgetGeometryResult Result;
    Result.ServedBy = EWidgetGeometrySource::Offscreen;

    // 1. ViewportSize always has a usable default (1920x1080) from FWidgetGeometryRequest.
    //    Auto mode (now the default when resolve_geometry is omitted) silently uses that
    //    default; explicit callers pass bViewportSizeProvided with their own size.

    UWidgetBlueprint* Blueprint = Request.Blueprint;
    if (!Blueprint)
    {
        Result.TopLevelError = TEXT("NULL_BLUEPRINT");
        return Result;
    }

    // 2. Get the generated class.
    UClass* GenClass = Blueprint->GeneratedClass;
    if (!GenClass)
    {
        Result.TopLevelError = TEXT("BLUEPRINT_NOT_COMPILED");
        return Result;
    }

    // 3. Pick a world.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        World = GWorld;
    }
    if (!World)
    {
        Result.TopLevelError = TEXT("NO_WORLD");
        return Result;
    }

    // 4. Create the widget. TStrongObjectPtr keeps it alive until we drop it.
    TStrongObjectPtr<UUserWidget> StrongRoot(CreateWidget<UUserWidget>(World, GenClass));
    UUserWidget* Root = StrongRoot.Get();
    if (!Root)
    {
        Result.TopLevelError = TEXT("CREATE_WIDGET_FAILED");
        return Result;
    }

    // 5a. Guard: if the tree contains a UUIExtensionPointWidget, skip the offscreen
    //     render entirely. That widget's RebuildWidget dereferences UCommonLocalPlayer
    //     which is null outside a live PIE session — proceeding would crash the editor.
    if (FWidgetGeometryResolver::ContainsExtensionPointWidget(Root->WidgetTree))
    {
        Result.TopLevelError = TEXT("UI_EXTENSION_POINT_REQUIRES_RUNTIME");
        // StrongRoot releases automatically; no Slate widget was built.
        return Result;
    }

    // 5b. If bForceVisibleForMeasure, flip Collapsed → SelfHitTestInvisible.
    //     We do not restore visibilities since the widget is torn down after measurement.
    if (Request.bForceVisibleForMeasure)
    {
        Root->WidgetTree->ForEachWidget([](UWidget* W)
        {
            if (W && W->GetVisibility() == ESlateVisibility::Collapsed)
            {
                W->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
            }
        });
    }

    // 6-7. Create a virtual window and attach the Slate widget.
    TSharedRef<SWidget> SlateWidget = Root->TakeWidget();

    const FVector2D& VS = Request.ViewportSize;
    TSharedRef<SVirtualWindow> VirtualWindow = SNew(SVirtualWindow).Size(VS);
    VirtualWindow->SetContent(SlateWidget);

    // 8. Run prepass at scale 1.0 so geometry is in layout pixels.
    VirtualWindow->SlatePrepass(1.0f);

    // 9. Populate the Slate geometry map by arranging children recursively.
    //    We start from the virtual window itself.
    TMap<TSharedPtr<const SWidget>, FGeometry> SlateGeometryMap;

    // Build geometry for the virtual window root and arrange downward.
    {
        FGeometry WindowRootGeometry = FGeometry::MakeRoot(
            FVector2f((float)VS.X, (float)VS.Y),
            FSlateLayoutTransform(1.0f));

        // Arrange the virtual window's children — this gives us SlateWidget's allotted geometry.
        FArrangedChildren WindowChildren(EVisibility::All);
        VirtualWindow->ArrangeChildren(WindowRootGeometry, WindowChildren);

        for (int32 i = 0; i < WindowChildren.Num(); ++i)
        {
            const FArrangedWidget& Child = WindowChildren[i];
            SlateGeometryMap.Add(Child.Widget, Child.Geometry);

            // Recursively populate the rest of the tree.
            // Pass Child.Geometry so the walk starts from the correct allotted geometry.
            PopulateSlateGeometryMap(Child.Widget, Child.Geometry, SlateGeometryMap);
        }
    }

    // Map every UWidget* → FGeometry via its cached Slate widget. Includes the root.
    auto EmitEntry = [&](UWidget* W)
    {
        if (!W) return;

        TSharedPtr<SWidget> Underlying = W->GetCachedWidget();
        FResolvedGeometry Geo;

        if (!Underlying.IsValid())
        {
            Geo.Status = FResolvedGeometry::EStatus::Error;
            Geo.Reason = TEXT("NOT_PAINTED");
            Result.ByWidget.Add(FObjectKey(W), Geo);
            return;
        }

        if (const FGeometry* FoundGeo = SlateGeometryMap.Find(Underlying))
        {
            FillResolvedGeometryFromMap(Geo, *FoundGeo, W, Underlying);
        }
        else
        {
            Geo.Status = FResolvedGeometry::EStatus::Error;
            Geo.Reason = TEXT("GEOMETRY_NOT_ARRANGED");
        }

        Result.ByWidget.Add(FObjectKey(W), Geo);
    };

    EmitEntry(Root);
    Root->WidgetTree->ForEachWidget(EmitEntry);

    // Detach Slate content so SVirtualWindow doesn't hold a dangling ref once it goes away.
    // StrongRoot is transferred to Result.LiveRootStrong so callers can dereference the root
    // (e.g. via BuildNameIndex) for the lifetime of the result.
    VirtualWindow->SetContent(SNullWidget::NullWidget);
    Result.LiveRootStrong = MoveTemp(StrongRoot);

    // 13. Fill result metadata.
    Result.DpiScale         = 1.0f;
    Result.ViewportSizeUsed = VS;
    Result.LiveRoot         = Root;

    return Result;
}

// ---------------------------------------------------------------------------
// FWidgetGeometryResolver::Resolve
// ---------------------------------------------------------------------------
FWidgetGeometryResult FWidgetGeometryResolver::Resolve(const FWidgetGeometryRequest& Request)
{
    if (!Request.Blueprint)
    {
        FWidgetGeometryResult Err;
        Err.TopLevelError = TEXT("NULL_BLUEPRINT");
        return Err;
    }

    // Auto: Designer → Live → Offscreen waterfall.

    // Tier A — Designer
    {
        FWidgetGeometryResult DesignerResult = ResolveViaDesigner(Request);
        if (DesignerResult.TopLevelError.IsEmpty() && DesignerResult.ByWidget.Num() > 0)
        {
            return DesignerResult;
        }
    }

    // Tier B — Live
    {
        FWidgetGeometryResult LiveResult = ResolveViaLive(Request);
        if (LiveResult.bAmbiguous)
        {
            // Return ambiguity info so the caller can decide.
            return LiveResult;
        }
        if (LiveResult.TopLevelError.IsEmpty() && LiveResult.ByWidget.Num() > 0)
        {
            return LiveResult;
        }
    }

    // Tier C — Offscreen (always reachable if viewport size was provided)
    return ResolveViaOffscreen(Request);
}

// ---------------------------------------------------------------------------
// FWidgetGeometryResolver::BuildNameIndex
// ---------------------------------------------------------------------------
void FWidgetGeometryResolver::BuildNameIndex(
    UUserWidget* LiveRoot,
    TMap<FName, UWidget*>& OutIndex)
{
    OutIndex.Reset();
    if (!LiveRoot)
    {
        return;
    }

    OutIndex.Add(LiveRoot->GetFName(), LiveRoot);
    if (UWidgetTree* Tree = LiveRoot->WidgetTree)
    {
        Tree->ForEachWidget([&OutIndex](UWidget* W)
        {
            if (W)
            {
                OutIndex.Add(W->GetFName(), W);
            }
        });
    }
}

// ---------------------------------------------------------------------------
// FWidgetGeometryResolver::LookupByName
// ---------------------------------------------------------------------------
const FResolvedGeometry* FWidgetGeometryResolver::LookupByName(
    const TMap<FObjectKey, FResolvedGeometry>& ByWidget,
    const TMap<FName, UWidget*>& NameIndex,
    FName WidgetName)
{
    UWidget* const* LiveWidgetPtr = NameIndex.Find(WidgetName);
    if (!LiveWidgetPtr || !*LiveWidgetPtr)
    {
        return nullptr;
    }
    return ByWidget.Find(FObjectKey(*LiveWidgetPtr));
}

// ---------------------------------------------------------------------------
// FWidgetGeometryResolver::SourceToString
// ---------------------------------------------------------------------------
FString FWidgetGeometryResolver::SourceToString(EWidgetGeometrySource Source)
{
    switch (Source)
    {
    case EWidgetGeometrySource::Designer:  return TEXT("designer");
    case EWidgetGeometrySource::Live:      return TEXT("live");
    case EWidgetGeometrySource::Offscreen: return TEXT("offscreen");
    case EWidgetGeometrySource::Auto:
    default:                               return TEXT("auto");
    }
}

// ---------------------------------------------------------------------------
// FWidgetGeometryResolver::ParseRequest
// ---------------------------------------------------------------------------
bool FWidgetGeometryResolver::ParseRequest(
    const TSharedPtr<FJsonValue>& GeoValue,
    UWidgetBlueprint* Blueprint,
    FWidgetGeometryRequest& OutRequest)
{
    OutRequest = FWidgetGeometryRequest{};
    OutRequest.Blueprint = Blueprint;

    // Geometry is off by default.
    if (!GeoValue.IsValid())
    {
        return false;
    }

    // bool: true → on with defaults; false → off.
    if (GeoValue->Type == EJson::Boolean)
    {
        return GeoValue->AsBool();
    }

    // object → on with fine control.
    if (GeoValue->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>& GeoParam = GeoValue->AsObject();

        const TSharedPtr<FJsonObject>* VpObj = nullptr;
        if (GeoParam->TryGetObjectField(TEXT("viewport_size"), VpObj) ||
            GeoParam->TryGetObjectField(TEXT("viewportSize"), VpObj))
        {
            if (VpObj && VpObj->IsValid())
            {
                double W = OutRequest.ViewportSize.X;
                double H = OutRequest.ViewportSize.Y;
                (*VpObj)->TryGetNumberField(TEXT("w"), W);
                (*VpObj)->TryGetNumberField(TEXT("h"), H);
                OutRequest.ViewportSize = FVector2D(W, H);
                OutRequest.bViewportSizeProvided = true;
            }
        }

        bool bForceVisible = false;
        if (GeoParam->TryGetBoolField(TEXT("force_visible_for_measure"), bForceVisible) ||
            GeoParam->TryGetBoolField(TEXT("forceVisibleForMeasure"), bForceVisible))
        {
            OutRequest.bForceVisibleForMeasure = bForceVisible;
        }

        FString InstanceName;
        if (GeoParam->TryGetStringField(TEXT("instance_name"), InstanceName) ||
            GeoParam->TryGetStringField(TEXT("instanceName"), InstanceName))
        {
            OutRequest.LiveInstanceName = InstanceName;
        }

        return true;
    }

    // Any other JSON type (string, number, array, null) — treat as disabled.
    return false;
}
