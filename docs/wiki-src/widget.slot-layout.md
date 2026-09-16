# Widget slot layout

How `CanvasPanelSlot.LayoutData`, Box slots, and common button/border/partial-struct patterns resolve
at runtime. Read this before writing `Slot.LayoutData` or debugging a 100×30 corner box.

## Canvas panel slot layout

Every `CanvasPanel` child has a `CanvasPanelSlot` serialized as one `LayoutData` struct:

```
(Offsets=(Left=X,Top=Y,Right=W,Bottom=H),
 Anchors=(Minimum=(X=..,Y=..),Maximum=(X=..,Y=..)),
 Alignment=(X=..,Y=..))
```

`Offsets` are **anchor-mode dependent**; there is no universal "Left/Top/Width/Height" meaning.
Determine the mode first.

**For each axis:**

- If `Anchors.Minimum.X == Anchors.Maximum.X`, it is a **point anchor**:
  - `Offsets.Left` = X position (from anchor)
  - `Offsets.Right` = Width
- If `Anchors.Minimum.X < Anchors.Maximum.X`, it **stretches**:
  - `Offsets.Left` = distance from min anchor (left margin)
  - `Offsets.Right` = distance from max anchor (right margin)

Apply the same rule to Y (`Top`/`Bottom`). `Alignment` shifts relative to the anchor; `(0.5, 0.5)`
centers on a point anchor.

**Default trap: child of CanvasPanel with no LayoutData**

A new CanvasPanel child defaults to **point anchor (0,0), Offsets (0,0,100,30)**: a 100×30
top-left box. An HBox with ten buttons then gets 100 px, so buttons compete and text overflows.

Whenever you add content to a CanvasPanel, set its slot explicitly:

```
widget.set widgetName=MyHBox slot={"LayoutData":
  "(Offsets=(Left=0,Top=0,Right=0,Bottom=0),
    Anchors=(Minimum=(X=0,Y=0),Maximum=(X=1,Y=1)),
    Alignment=(X=0,Y=0))"}
```

This fills the canvas; use non-zero offsets for a margin.

**Bottom-pinned fixed-height bar**

Common pattern: a full-width toolbar pinned to the bottom, N pixels tall:

```
Anchors=(Minimum=(X=0,Y=1),Maximum=(X=1,Y=1))
Alignment=(X=0,Y=0)
Offsets=(Left=0, Top=-N, Right=0, Bottom=N)
```

X stretches full width; Y is a bottom point anchor (`y=1`). `Offsets.Top=-N` positions it above the
bottom and `Offsets.Bottom=N` is its height.

Do not use percent-Y anchors for fixed-size chrome (toolbars, title bars): it cramps at low resolution
and bloats at high resolution. Use fixed heights.

**Floating left-anchored panel**

```
Anchors=(Minimum=(X=0,Y=0.5),Maximum=(X=0,Y=0.5))
Alignment=(X=0,Y=0.5)
Offsets=(Left=30, Top=0, Right=260, Bottom=440)
```

Left edge at 50% height, vertically centered: 30 px from left, size 260×440.

## Box slot layout

`VerticalBox` / `HorizontalBox` children use a simpler slot struct:

- `Padding` = `(Left=x,Top=x,Right=x,Bottom=x)` — `FMargin`.
- `HorizontalAlignment` / `VerticalAlignment` = `HAlign_Left` | `HAlign_Center` | `HAlign_Right` | `HAlign_Fill` (and V equivalents).
- `Size` = `(SizeRule=Auto)` (default — fit to content) or `(SizeRule=Fill,Value=1.0)` (expand into leftover space, weighted by `Value`).

`VerticalBox` lays children top-to-bottom and is Auto-sized by default. `HAlign_Fill` expands a child
to the box width; `HAlign_Center` centers an auto-width child. `Size=Fill` affects only the major
axis (vertical for VBox).

**Centering children inside a stretched HBox**

A HorizontalBox does not center children by default. If it is 800 px wide and children total 600 px,
the 200 px remainder stays on the right. Add start/end `Spacer` children with
`Size=(SizeRule=Fill, Value=1.0)`:

```
widget.add type=Spacer name=LeftSpacer parentName=MyHBox
widget.set widgetName=LeftSpacer slot={"Size": "(SizeRule=Fill,Value=1.0)"}
widget.add type=Spacer name=RightSpacer parentName=MyHBox
widget.set widgetName=RightSpacer slot={"Size": "(SizeRule=Fill,Value=1.0)"}
```

Spacers are invisible and absorb remainder. `widget.add` appends, so for
`[LeftSpacer, ...content, RightSpacer]` add the left spacer before content or reorder with
`widget.reparent_widget`.

## Widget content — buttons, borders, partial structs

**Buttons need a TextBlock child**

A bare `Button` renders only its style background; it has no label. Add a `TextBlock` child:

```
widget.add widgetPath=<bp> type=TextBlock name=MyText parentName=MyButton
widget.set widgetPath=<bp> widgetName=MyText properties={"Text": "Save"}
```

Buttons accept one child; use a `HorizontalBox` child for icon + text.

**RoundedBox — the HalfHeightRadius default**

`Border.Background` with `DrawAs=RoundedBox` honors `OutlineSettings.RoundingType`:

- `FixedRadius` — `CornerRadii` (FVector4) is pixel radius per corner.
- `HalfHeightRadius` — `CornerRadii` clamps to `min(value, height/2)`; larger values produce a pill/blob.

`HalfHeightRadius` is **the default**. With `CornerRadii=(16,16,16,16)` on a 400 px border and no
`RoundingType=FixedRadius`, corners become 200 px (a dome). Set `RoundingType` explicitly for a
rounded rectangle:

```
"OutlineSettings=(CornerRadii=(X=12,Y=12,Z=12,W=12),
                  Color=(SpecifiedColor=(R=0,G=0,B=0,A=0)),
                  RoundingType=FixedRadius)"
```

**Partial structs are OK**

UE property serialization accepts partial struct fragments; write only fields to override.

- `FSlateColor`: `(SpecifiedColor=(R=0.95,G=0.55,B=0.15,A=1.0))`
- `FSlateFontInfo`: `(Size=16,TypefaceFontName="Default")`
- `FLinearColor`: `(R=1,G=0.5,B=0,A=1)`

Omitted fields retain their previous values.

## See also

- [`widget`](widget.md) — the UMG authoring namespace these slot recipes are written against.
- [`widget.xml-markup`](widget.xml-markup.md) — the XML dialect the layouts above are expressed in.
- [`widget.failure-modes`](widget.failure-modes.md) — geometry and resolver limitations to expect.
