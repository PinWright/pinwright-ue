# object

Generic UObject UFunction-invocation escape hatch — `object.call_function` invokes any reflected `UFunction` on a resolved UObject by name with JSON-coerced arguments. It now covers subsystems (resolve their object paths via `call("system.inspect.list_subsystems")`) and actors (paths come from `actor.*` query responses) as well as arbitrary UObjects with no dedicated typed handler.

## See also

- [`runtime-uobject-inspection`](runtime-uobject-inspection.md) — the workflow for reading and driving live PIE UObjects.
- [`system.inspect`](system.inspect.md) — resolving the subsystem and object paths to call against.
- [`property`](property.md) — reading and writing reflected properties instead of invoking a function.
