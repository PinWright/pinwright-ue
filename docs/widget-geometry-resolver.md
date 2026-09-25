---
type: system
summary: "FWidgetGeometryResolver: tiered UMG widget geometry measurement (Designer → Live → Offscreen), public API, SVirtualWindow offscreen pattern, consumers widget.describe and widget.export_xml, coordinate system, BuildNameIndex O(1) lookup."
date: 2026-09-24
tags: [widget, geometry, umg, slate, offscreen, fgeometry, widget-describe, widget-export-xml]
---

# Widget Geometry Resolver

`FWidgetGeometryResolver` (`Private/Handlers/UI/WidgetGeometryResolver.h/.cpp`) resolves window-relative `FGeometry` values for every widget in a `UWidgetBlueprint` tree. It is consumed by `widget.describe` and `widget.export_xml` to emit per-node geometry data.

## Tiered Resolution Strategy

The resolver tries three sources in order when `PreferredSource == Auto` (the default). Each tier returns early with a non-empty `ByWidget` map and no `TopLevelError` on success; the next tier is tried otherwise.

### Tier A — Designer

If the `UWidgetBlueprint` is currently open in the Widget Blueprint Editor, the resolver reads `FWidgetBlueprintEditor::GetPreview()->GetCachedGeometry()`. This matches exactly what the Designer tab shows, including any pinned size set in the designer.

The editor instance is found via `UAssetEditorSubsystem::FindEditorForAsset(..., bFocusIfOpen=false)` followed by `dynamic_cast<FWidgetBlueprintEditor*>`. Note: there is no `UObject`-based `Cast<>` for `IAssetEditorInstance` — you must use a C++ `dynamic_cast`.

### Tier B — Live

If a `UUserWidget` of the generated class is live on any game or editor viewport, the resolver reads the running instance's cached geometry. Candidates are collected from both `GEditor->GetEditorWorldContext().World()` and `GEngine->GameViewport->GetWorld()` — mirroring the world-search pattern in `UiHandler.cpp:341-345`.

A widget is accepted as a candidate only when `IsInViewport() && GetCachedWidget().IsValid() && LocalSize > 0`. When multiple instances exist and the caller did not supply `LiveInstanceName` to narrow the match, the resolver returns `AMBIGUOUS_INSTANCE` with the full candidate list — it never auto-picks one.

`GEngine->GameViewport->GetWorld()` requires an explicit `#include "Engine/GameViewportClient.h"`. `Engine/Engine.h` only forward-declares `UGameViewportClient`, so accessing any member without the full header causes C2027/C2039 despite the pointer chain appearing complete.

### Tier C — Offscreen (always-available fallback)

The offscreen tier is always available. It:

1. Constructs a transient **design-time** `UUserWidget` in the editor world, the way the UMG Designer builds its preview: `NewObject` + `SetDesignerFlags(EWidgetDesignFlags::Designing)` before and after `Initialize()`. `IsDesignTime()` makes `Initialize` skip `NativeOnInitialized` and `OnWidgetRebuilt` skip `NativePreConstruct`/`NativeConstruct`, so a C++ parent's runtime lifecycle never runs without a game instance. It used `CreateWidget<>` (a runtime instance) until `B-geometry-offscreen-runs-native-construct`: a project parent's `NativeConstruct` reached `UGameplayMessageSubsystem::Get`, which asserts with no router, and killed the editor. Abstract / deprecated / superseded generated classes return `CREATE_WIDGET_FAILED`, as `CreateWidget` did. Pinned by `PinWright.widget_geometry.offscreen.DoesNotRunNativeConstruct`.
2. Calls `UUserWidget::TakeWidget()` to obtain the root `SWidget`.
3. Creates an `SVirtualWindow` sized to `Request.ViewportSize` (default 1920×1080) and sets it as the window's content.
4. Calls `VirtualWindow->SlatePrepass(1.0f)` to run the desired-size pass.
5. Recursively arranges children via `SWidget::ArrangeChildren(..., FArrangedChildren(EVisibility::All))`, starting from `FGeometry::MakeRoot(Size, FSlateLayoutTransform(1.0))` at the virtual window root.
6. Stores the resulting `FGeometry` values in a `TMap<TSharedPtr<const SWidget>, FGeometry>`.
7. Maps each `UWidget*` back to its Slate widget via `GetCachedWidget()` for lookup.

This pattern mirrors `SDesignerView::PopulateWidgetGeometryCache_Loop` at `C:\UE_5.6\Engine\Source\Editor\UMGEditor\Private\Designer\SDesignerView.cpp:2034-2089`. `FSlateApplication::Tick` is **not** required for a geometry-only measurement pass.

The transient `UUserWidget` is kept alive beyond `Resolve()` by transferring a `TStrongObjectPtr<UUserWidget>` into `FWidgetGeometryResult::LiveRootStrong`. This prevents premature GC during the caller's use of the result.

## Public API

### `Resolve(FWidgetGeometryRequest)` → `FWidgetGeometryResult`

`FWidgetGeometryRequest` fields:
- `ViewportSize` (`FVector2D`) — window dimensions for the offscreen tier (default 1920×1080).
- `bForceVisibleForMeasure` (`bool`) — flip Collapsed → SelfHitTestInvisible during offscreen measurement.
- `LiveInstanceName` (`FString`) — disambiguator when multiple live instances exist.

`Resolve()` always runs the Designer → Live → Offscreen waterfall. The `ServedBy` result field reports which tier actually produced the data.

`FWidgetGeometryResult` fields:
- `ByWidget` (`TMap<FObjectKey, FResolvedGeometry>`) — per-widget geometry keyed by `FObjectKey(UWidget*)`.
- `GetLiveRoot()` accessor — returns the root `UUserWidget*` from the offscreen tier (kept alive via `LiveRootStrong`).
- `ServedBy` (`EWidgetGeometrySource`) — which tier actually produced the result.
- `ViewportSizeUsed` (`FVector2D`).
- `DpiScale` (`float`).
- `bAmbiguous` (`bool`) + `AmbiguousCandidates` (`TArray<FString>`) — set when the Live tier found multiple instances.
- `TopLevelError` (`FString`) — non-empty when no tier succeeded.

### Companion Statics

| Method | Purpose |
|---|---|
| `BuildNameIndex(LiveRoot, TMap<FName, UWidget*>&)` | Builds a flat name→widget map for O(1) per-node lookup during tree walks. Required before `LookupByName`. `UUserWidget::GetWidgetFromName` is O(N) — it calls `WidgetTree->FindWidget` which iterates all widgets — so calling it once per node in a tree walk is O(N²). |
| `LookupByName(ByWidget, NameIndex, FName)` | Resolves a widget by name using the pre-built index. |
| `SourceToString(EWidgetGeometrySource)` | Converts the source enum to a JSON-safe string. |
| `ParseRequest(FJsonValue GeoValue, UWidgetBlueprint*, FWidgetGeometryRequest&) -> bool bEnabled` | Shared `resolve_geometry` param parser used by both `widget.describe` and `widget.export_xml` handlers. Accepts bool or object; returns `false` when geometry is disabled (off by default). |
| `ContainsExtensionPointWidget(UWidgetTree*)` | Returns true when the tree contains a `UUIExtensionPointWidget`. `ResolveViaOffscreen` uses this as a crash guard — that widget's `RebuildWidget` dereferences `UCommonLocalPlayer`, which is null outside a live PIE session. |

## Coordinate System

All geometry values are **post-DPI Slate absolute units** — identical to what the Widget Reflector and UMG Designer rulers display. These come directly from `FGeometry::GetAbsolutePosition()` / `GetAbsoluteSize()`. The `dpi_scale` value is reported at the root so callers can convert to logical pixels if needed.

## FWidgetTransform Layout

`FWidgetTransform` (UE 5.6) has exactly four fields: `Translation` (`FVector2D`), `Scale` (`FVector2D`), `Shear` (`FVector2D`), and `Angle` (`float`). There is no `Pivot` member — the render pivot lives on `UWidget::RenderTransformPivot` as a separate property. When serializing render transforms, always read `RenderTransformPivot` from `UWidget` directly rather than looking for it inside `FWidgetTransform`.

## Consumers

Both consuming handlers call `FWidgetGeometryResolver::ParseRequest` to extract the `resolve_geometry` param, then call `Resolve()` if enabled.

- **`WidgetDescribeHandler.cpp`** (`widget.describe`) — adds a `geometry` JSON object per node. Top-level response fields: `geometry_source`, `viewport_size`, `dpi_scale`, `geometry_summary`.
- **`WidgetXmlExportHandler.cpp`** (`widget.export_xml`) — adds `Geom.*` attributes to each XML element. Non-identity render transforms emit a `<RenderTransform>` child element.

Geometry emission defaults to **off**. Callers opt in by passing `resolve_geometry: true` (defaults) or an object `{viewport_size, force_visible_for_measure, instance_name}` for fine control.

For the full wire format (JSON field names, XML attribute names, reason codes), see the user-facing documentation in [mcp-usage-guide](wiki-src/widget.md) § Widget Geometry Resolution.

## See Also

- [mcp-usage-guide](wiki-src/widget.md) — User-facing wire format: JSON/XML output shapes, param reference, reason codes, back-compat note
- [arch](arch.md) — Plugin architecture: 7-layer design (incl. Jobs), Handler layer conventions
- [lessons](lessons.md) — Corrective patterns including `GetWidgetFromName` O(N) cost, `FWidgetTransform` fields, offscreen measurement pattern, `GameViewportClient.h` include requirement
