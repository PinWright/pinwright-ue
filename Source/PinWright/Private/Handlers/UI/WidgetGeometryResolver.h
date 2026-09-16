// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/ObjectKey.h"
#include "UObject/StrongObjectPtr.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Layout/Geometry.h"
#include "Types/SlateEnums.h"

#include "WidgetBlueprint.h"

class FWidgetBlueprintEditor;
class SWidget;
class SWindow;

// ---------------------------------------------------------------------------
// FResolvedGeometry
// Per-widget geometry result produced by FWidgetGeometryResolver.
// ---------------------------------------------------------------------------
struct FResolvedGeometry
{
    enum class EStatus : uint8
    {
        Ok,
        Skipped,
        Error,
    };

    EStatus Status = EStatus::Error;

    // Human-readable reason when Status != Ok; empty on Ok.
    FString Reason;

    // FGeometry::GetAbsolutePosition(). DESKTOP-space when the source geometry came from a
    // painted widget's cached/tick-space geometry (SWidget::GetCachedGeometry ->
    // GetTickSpaceGeometry -> PersistentState.DesktopGeometry, which SWidget::Paint stores with
    // the owning window's GetPositionInScreen() already appended); window-space only for a
    // geometry taken straight out of an in-progress paint/arrange pass.
    FVector2D AbsolutePos = FVector2D::ZeroVector;

    // Absolute size in render space: FGeometry::GetAbsoluteSize() (LocalSize * accumulated scale, no render-transform inflation).
    FVector2D AbsoluteSize = FVector2D::ZeroVector;

    // Local (layout) size from FGeometry::GetLocalSize().
    FVector2D LocalSize = FVector2D::ZeroVector;

    // SWidget::GetDesiredSize() when a cached Slate widget is available; otherwise zero.
    FVector2D DesiredSize = FVector2D::ZeroVector;

    // Effective visibility (Slate runtime enum) converted from UWidget::GetVisibility().
    // Use UWidget::ConvertSerializedVisibilityToRuntime() to translate from ESlateVisibility.
    EVisibility EffectiveVisibility = EVisibility::Visible;

    // True if the widget has a non-identity render transform.
    bool bHasRenderTransform = false;

    // Full render transform when bHasRenderTransform is true.
    FWidgetTransform RenderTransformValue;
};

// ---------------------------------------------------------------------------
// EWidgetGeometrySource
// Which measurement tier actually produced a result (used for ServedBy reporting).
// ---------------------------------------------------------------------------
enum class EWidgetGeometrySource : uint8
{
    Auto,        // Result produced by the waterfall (Designer → Live → Offscreen).
    Designer,    // Result came from the Widget Blueprint Editor preview.
    Live,        // Result came from a live on-screen instance.
    Offscreen,   // Result came from the virtual-window offline render.
};

struct PINWRIGHT_API FWidgetDesignerPreviewTarget
{
    FWidgetBlueprintEditor* Editor = nullptr;
    UUserWidget* Preview = nullptr;
    TSharedPtr<SWidget> PreviewSlate;
    // SDesignerView anchor used for Slate screenshots; PreviewCropRect narrows it to the
    // rendered preview canvas so designer rulers/toolbars stay out of target:"preview".
    TSharedPtr<SWidget> DesignerViewSlate;
    FGeometry DesignerViewGeometry;
    FGeometry PreviewGeometry;
    FIntRect PreviewCropRect = FIntRect(0, 0, 0, 0);
    bool bHasPreviewCropRect = false;
    TSharedPtr<SWindow> HostWindow;
    FGeometry RootGeometry;
    float DpiScale = 1.0f;
};

// ---------------------------------------------------------------------------
// FWidgetGeometryRequest
// Input to FWidgetGeometryResolver::Resolve().
// ---------------------------------------------------------------------------
struct FWidgetGeometryRequest
{
    // The widget blueprint to measure. Must not be null.
    UWidgetBlueprint* Blueprint = nullptr;

    // Viewport size used by the Offscreen tier. Ignored by Designer/Live tiers.
    FVector2D ViewportSize = FVector2D(1920.0, 1080.0);

    // Caller sets this to true when ViewportSize was explicitly provided (rather than
    // defaulted). Currently informational only — the Offscreen tier always uses whatever
    // size is in ViewportSize — but may affect future heuristics.
    bool bViewportSizeProvided = false;

    // When true and Offscreen tier is used, collapsed widgets are temporarily
    // set to SelfHitTestInvisible before measuring (so sizes are non-zero).
    bool bForceVisibleForMeasure = false;

    // Optional name to disambiguate when multiple live instances exist (Live tier).
    // If non-empty, only the instance whose outer-chain name contains this string is used.
    FString LiveInstanceName;
};

// ---------------------------------------------------------------------------
// FWidgetGeometryResult
// Output of FWidgetGeometryResolver::Resolve().
// ---------------------------------------------------------------------------
struct FWidgetGeometryResult
{
    // Which tier actually produced this result.
    EWidgetGeometrySource ServedBy = EWidgetGeometrySource::Auto;

    // The viewport/window size that was used (filled in by each tier).
    FVector2D ViewportSizeUsed = FVector2D::ZeroVector;

    // DPI scale factor of the window that served the result (1.0 for Offscreen).
    float DpiScale = 1.0f;

    // Per-widget geometry keyed by the live UWidget* pointer that was measured.
    // For Designer/Live tiers: keys are pointers inside the live preview/instance tree.
    // For Offscreen tier: keys are pointers inside the freshly created widget tree.
    // Callers that have asset-tree UWidget* pointers should use LookupByName().
    TMap<FObjectKey, FResolvedGeometry> ByWidget;

    // True when Live tier found multiple matching instances and LiveInstanceName
    // did not narrow it down to exactly one.
    bool bAmbiguous = false;

    // Populated when bAmbiguous is true.
    TArray<TWeakObjectPtr<UUserWidget>> AmbiguousCandidates;

    // Non-empty when a fatal error prevented any measurement (e.g. blueprint not compiled).
    FString TopLevelError;

    // Preferred accessor — returns the live/preview/offscreen UUserWidget root the caller
    // should pass to BuildNameIndex/LookupByName. Transparently prefers the Offscreen
    // tier's keep-alive strong ptr when present, otherwise returns the weak ptr's target.
    UUserWidget* GetLiveRoot() const
    {
        return LiveRootStrong.IsValid() ? LiveRootStrong.Get() : LiveRoot.Get();
    }

    // Weak pointer to the root for Designer/Live tiers. Internal — external callers
    // should use GetLiveRoot().
    TWeakObjectPtr<UUserWidget> LiveRoot;

    // Offscreen tier only: keeps the ephemeral transient widget alive for the lifetime
    // of this result. Implementation detail — not meant to be read by callers.
    TStrongObjectPtr<UUserWidget> LiveRootStrong;
};

// ---------------------------------------------------------------------------
// FWidgetGeometryResolver
// Tiered resolver: Designer → Live → Offscreen.
// ---------------------------------------------------------------------------
class PINWRIGHT_API FWidgetGeometryResolver
{
public:
    /**
     * Resolve widget geometry for all widgets inside Request.Blueprint.
     * Always runs the Designer → Live → Offscreen waterfall.
     */
    static FWidgetGeometryResult Resolve(const FWidgetGeometryRequest& Request);

    static FWidgetBlueprintEditor* FindWidgetBlueprintEditor(
        UWidgetBlueprint* Blueprint,
        bool bFocusIfOpen);

    // bResolveDesignerSurface: when true, populate OutTarget.DesignerViewSlate and the
    // preview crop bounds via the SDesignerView lookup. Window-mode capture callers can
    // pass false to skip this since they don't consume preview bounds.
    static bool ResolveDesignerPreviewTarget(
        UWidgetBlueprint* Blueprint,
        bool bFocusIfOpen,
        FWidgetDesignerPreviewTarget& OutTarget,
        FString* OutError = nullptr,
        bool bResolveDesignerSurface = true);

    /**
     * Build an FName → UWidget* lookup table for the given live root. Used by
     * LookupByName to resolve asset-tree FNames to live-tree widget pointers in O(1).
     * Includes both the root and every named child in the widget tree.
     */
    static void BuildNameIndex(
        UUserWidget* LiveRoot,
        TMap<FName, UWidget*>& OutIndex);

    /**
     * Look up geometry for a named widget in a result map using a pre-built name index.
     * O(1). Returns nullptr when the name is not in the index or the live widget has
     * no entry in ByWidget.
     */
    static const FResolvedGeometry* LookupByName(
        const TMap<FObjectKey, FResolvedGeometry>& ByWidget,
        const TMap<FName, UWidget*>& NameIndex,
        FName WidgetName);

    /**
     * Convert an EWidgetGeometrySource to its JSON/XML wire string.
     * ("designer" | "live" | "offscreen" | "auto")
     */
    static FString SourceToString(EWidgetGeometrySource Source);

    /**
     * Parse a `resolve_geometry` value into a FWidgetGeometryRequest.
     *
     * Geometry is off by default. Passing null or an invalid value disables geometry.
     * Callers opt in explicitly:
     *   - bool true  → geometry on with defaults (1920x1080, auto waterfall).
     *   - bool false → geometry off.
     *   - object     → geometry on with fine control:
     *                  {viewport_size: {w, h}, force_visible_for_measure: bool,
     *                   instance_name: string}.
     *   - null / omitted / any other type → geometry off.
     *
     * @param GeoValue    The raw JSON value for the `resolve_geometry` parameter.
     * @param Blueprint   The widget blueprint to measure; stored on OutRequest.Blueprint.
     * @param OutRequest  Populated with ViewportSize, flags, etc.
     * @return true when geometry emission is enabled; false when disabled (default off).
     */
    static bool ParseRequest(
        const TSharedPtr<FJsonValue>& GeoValue,
        UWidgetBlueprint* Blueprint,
        FWidgetGeometryRequest& OutRequest);

    /**
     * Walk the given widget tree and return true if any widget's class path matches
     * /Script/UIExtension.UIExtensionPointWidget.
     * Detection uses the class path string so this module does not need to depend on
     * the UIExtension module at link time.
     * Walks the parent tree AND every nested UUserWidget's WidgetTree (via
     * ForEachWidgetAndDescendants), matching the offscreen TakeWidget rebuild cascade.
     */
    static bool ContainsExtensionPointWidget(UWidgetTree* Tree);
};
