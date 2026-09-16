# bpir.examples.async-k2node

## Async K2Node Call with Exec Branches

Some UE functions have both sync `UFUNCTION` and async `K2Node` forms. Plain `call FunctionName(...)` can resolve to the async-action factory return (`UAsyncAction_*`), **not** the intended widget or value. Use the exact `call K2Node_AsyncAction_<FactoryFunctionName>(...)` form to expose exec-branch pins and `%action.UserWidget` on the async-action result.

This pattern is required for `PushContentToLayerForPlayer`, which must expose `AfterPush` to get the widget reference.

```
entry custom_event ShowError(text ErrorMessage) {
    # plain call: resolves to UAsyncAction_* return, no widget access
    # %action = call PushContentToLayerForPlayer(...)  -- WRONG

    # K2Node form: exposes AfterPush exec and action.UserWidget
    %action = call K2Node_AsyncAction_PushContentToLayerForPlayer(
        OwningPlayer: $PC,
        WidgetClass: "/Game/UI/W_Error.W_Error_C",
        LayerName: (TagName="UI.Layer.Game"),
        bSuspendInputUntilComplete: false
    ) [AfterPush -> @pushed]

@pushed:
    %cast = cast<W_Error_C>(%action.UserWidget) [success -> @ok, fail -> @end]

@ok:
    call `Set Error`(Target: %cast.AsWError, ErrorText: Conv_TextToString($ErrorMessage))

@end:
}
```

Bare `call K2Node_AsyncAction(...)` is rejected. BPIR no longer infers an async factory from pin names; use the exact factory-function suffix so decompile/recompile preserve the configured node identity. See `call("bpir.instructions")` §2.1 for `call` syntax and `call("bpir.errors")` for the "Could not find target pin" diagnostic when factory pins are mistyped.

> **Tests:** Covered by `PinWright.bpir.round_trip.AsyncActionFactoryIdentity`.

_See also: call("bpir.examples") for the full index._
