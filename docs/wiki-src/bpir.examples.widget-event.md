# bpir.examples.widget-event

## Widget Event

Bind to a UMG widget event using `widget_event`.

Prerequisite: the target widget name must exist in the Widget Blueprint tree; inspect it with `call("widget.describe")` or `call("widget.export_xml")`. After compiling the BPIR event entry, verify or refresh the binding with `call("widget.bind_event")`.

```
entry widget_event PlayButton.OnClicked() {
    call RemoveFromParent(Target: self)
}
```

> **Tests:** Parser: `ParseWidgetEventEntry` | Compiler: `WidgetEventEntry` | Decompiler: _no dedicated test_ | Round-trip: _no dedicated test_

_See also: call("bpir.examples") for the full index._
