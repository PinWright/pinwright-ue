# bpir.examples.backtick-quoted-names

## Backtick-Quoted Function Names with Spaces

BP functions whose names contain spaces must be wrapped in backticks at both the declaration site and all call sites. Plain unquoted names fail the parser (splits on the space token). Double-quoted names fail the resolver (quotes become part of the resolved name). Backticks are the only accepted quoting form.

The BPIR decompiler automatically emits backticks whenever a UFunction's display name contains a space, so decompiled output always round-trips correctly.

```
entry function `Set Error`(object<W_Error_C> Target, string ErrorText) {
    call SetText(Target: $Target.ErrorLabel, InText: Conv_StringToText($ErrorText))
}

entry event BeginPlay() {
    %w = call GetWidgetFromName(WidgetName: "ErrorWidget")
    %cast = cast<W_Error_C>(%w) [success -> @ok, fail -> @end]

@ok:
    # Call the spaced-name function via backtick quoting
    call `Set Error`(Target: %cast.AsWError, ErrorText: "Something went wrong")

@end:
}
```

The space-stripped resolver fallback also accepts `SetError` and `Set_Error` as aliases when no ambiguity exists, but backtick quoting is the canonical and unambiguous form. See `call("bpir.entry-points")` §1 "Backtick-quoted names" for the full rules.

> **Tests:** No dedicated round-trip test for spaced-name functions yet. Backtick parsing is exercised implicitly by production widget wiring.

_See also: call("bpir.examples") for the full index._
