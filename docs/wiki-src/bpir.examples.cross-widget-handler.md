# bpir.examples.cross-widget-handler

## Cross-Widget Handler Discovery

When a widget cannot hold a direct variable reference to a sibling handler widget (e.g., a results modal that is spawned after the handler), use `GetAllWidgetsOfClass → foreach → cast` to locate the shared handler at runtime. The dispatcher bind on the located handler uses the external `Target:` form documented in `call("bpir.instructions")` §2.10.

The cast accessor strips underscores from the class-name suffix: `W_ReplaySaveHandler_C` → `.AsWReplaySaveHandler`.

```
entry event Construct() {
    # Discover the shared handler widget — no direct variable reference available
    %found = call GetAllWidgetsOfClass(WidgetClass: "/Game/UI/W_ReplaySaveHandler.W_ReplaySaveHandler_C")
    %loop = foreach(%found) [body -> @body, completed -> @done]

@body:
    %cast = cast<W_ReplaySaveHandler_C>(%loop.ArrayElement) [success -> @ok, fail -> @done]

@ok:
    # Bind to the handler's dispatcher
    bind_dispatcher %cast.AsWReplaySaveHandler.OnSaveStateChanged(target: self, event: @OnHandlerStateChanged)
    # Trigger initial state update
    call OnHandlerStateChanged(State: enum EReplaySaveState::NotAvailable)

@done:
}

entry custom_event OnHandlerStateChanged(enum EReplaySaveState State) {
    # Update button visibility/enabled state based on state
    %sw = switch_enum<EReplaySaveState>($State) [Ready -> @ready, Uploading -> @uploading, default -> @unavailable]

@ready:
    call SetVisibility(Target: $SaveButtonContainer, InVisibility: enum ESlateVisibility::Visible)
    exec -> @end

@uploading:
    call SetIsEnabled(Target: $SaveButton, bInIsEnabled: false)
    exec -> @end

@unavailable:
    call SetVisibility(Target: $SaveButtonContainer, InVisibility: enum ESlateVisibility::Collapsed)

@end:
}
```

> **Tests:** No dedicated test yet. Pattern exercised by production widget wiring.

_See also: call("bpir.examples") for the full index._
