# object

Generic UObject UFunction-invocation escape hatch — `object.call_function` invokes any reflected `UFunction` on a resolved UObject by name with JSON-coerced arguments. It now covers subsystems (resolve their object paths via `call("system.inspect.list_subsystems")`) and actors (paths come from `actor.*` query responses) as well as arbitrary UObjects with no dedicated typed handler.

## See also

- [`runtime-uobject-inspection`](runtime-uobject-inspection.md) — the workflow for reading and driving live PIE UObjects.
- [`system.inspect`](system.inspect.md) — resolving the subsystem and object paths to call against.
- [`property`](property.md) — reading and writing reflected properties instead of invoking a function.

### object.call_function

**RPCs follow the engine's routing for PIE objects.** For a target in a game world (PIE), the call runs with `GAllowActorScriptExecutionInEditor` cleared, so `Server`/`Client`/`NetMulticast` RPCs take the normal callspace: a `Server` RPC called on a client-world object is sent to the server and its `_Implementation` runs there, not on the client. The response returns as soon as the RPC is queued, so read the server's copy after a net tick before treating the change as applied. For editor-world and world-less targets (assets, subsystems) the call runs under `FEditorScriptExecutionGuard`, which is what lets editor-world actors execute reflected functions at all; RPCs there run locally. `python.execute` cannot give you the PIE behaviour: the Python plugin forces every UFUNCTION it calls to run its RPCs locally (see [`python`](python.md) → "Calls that crash the editor").
