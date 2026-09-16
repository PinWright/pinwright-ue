# networking

Configure Blueprint replication, RPC functions, actor relevance, prediction, and current-world network settings — replicated variables, RepNotify hooks, replication conditions, movement replication, RPC reliability/validation, owner state, and authority/local-control checks. Use this for replicated Blueprint assets or named actors in the current editor world; use `call("session")` for LAN session hosting, joining, local-player, split-screen, and voice configuration.

### networking.create_rpc_function

`rpcType` accepts `Server`, `Client`, or `NetMulticast` case-insensitively. Each creates a function with `FUNC_Net` plus exactly that direction flag; `reliable:true` additionally sets `FUNC_NetReliable`, while `reliable:false` leaves it clear. Any other direction returns `INVALID_RPC_CONFIGURATION` before the Blueprint is loaded.

Optional `inputs[]` use the same `{name, type}` contract as `blueprint.add_function`. Inputs are authored as output pins on the function-entry node, which is Unreal's graph representation of function parameters. Bare strings, missing/empty fields, wrong field types, and extra keys return `INVALID_ARGUMENT` before the Blueprint is loaded; they are not silently skipped. RPCs cannot return values, so a non-empty `outputs[]` is refused before mutation with `INVALID_RPC_CONFIGURATION`.

`networking.set_rpc_reliability` and `networking.configure_rpc_validation` require the target graph to carry `FUNC_Net` and exactly one of the three direction flags. Blueprint-authored RPCs cannot safely enable `FUNC_NetValidate`: Unreal implements that flag through a native thunk that calls a C++ `_Validate` implementation, which a Blueprint function graph cannot supply. For `configure_rpc_validation`, `withValidation` must be false or omitted to clear a stale flag; `withValidation:true` returns `UNSUPPORTED` without changing the function.

### networking.configure_movement_prediction

`networkSmoothingMode` writes the character movement component's engine `NetworkSmoothingMode` setting. It accepts `Disabled`, `Linear`, or `Exponential` case-insensitively and defaults to `Exponential`; any other value returns `INVALID_ARGUMENT` before the Blueprint is loaded. The same call writes `networkMaxSmoothUpdateDistance` and `networkNoSmoothUpdateDistance`, whose defaults are `256` and `384`.

### networking.get_networking_info

The namespace's lone structured **reader** — the round-trip confirm for the
replication / RPC / relevance / role setters. Its parameters are branch
selectors (pass **exactly one**), and **the returned fields differ by branch**,
so consult this before assuming a given setter's state is confirmable here.

Returns `{ success, networkingInfo: { … } }`. Provide **one** of:

- **`blueprintPath`** — a Blueprint asset path. Reads the generated-class CDO
  plus the authored function graphs / replicated variables. `networkingInfo`
  carries:
  - the 8 actor-CDO fields `bReplicates`, `bAlwaysRelevant`,
    `bOnlyRelevantToOwner`, `netUpdateFrequency`, `minNetUpdateFrequency`,
    `netCullDistanceSquared`, `netPriority`, `netDormancy` — present **only when
    the blueprint's generated class is an actor** (its CDO is an `AActor`). A
    non-actor blueprint (widget BP, data-only BP) omits all 8 fields and returns
    just the two arrays below, still with `success: true` — so their *absence* is
    the "not an actor" signal, not a `false` value;
  - `rpcFunctions[]` — one entry per RPC (`FUNC_Net`) function, each with
    `name`, `rpcType` (`Server` / `Client` / `NetMulticast`), `reliable`, and
    `withValidation` (the RPC's *WithValidation* flag — surfaced by no other reader);
  - `replicatedProperties[]` — one entry per replicated variable, each with
    `name`, `replicated`, `replicatedUsing` (the RepNotify function name, or
    `null` when none), and `replicationCondition` (e.g. `COND_OwnerOnly`).
  - It does **not** return `role` / `remoteRole` / `hasAuthority` or `owner` —
    those describe a live instance, not a class; use the `actorName` branch for them.
- **`actorName`** — the internal object name of a placed actor in the current
  editor world. Reads the live instance. `networkingInfo` carries:
  - the same 8 CDO fields, **plus** `role`, `remoteRole`, `hasAuthority`, and
    `owner` / `ownerName` (both `null` when the actor is unowned — the only
    structured read-back of a `set_owner` / `actor.attach` / `actor.detach` change).
  - It does **not** return `rpcFunctions[]` / `replicatedProperties[]` — a placed
    actor exposes no authored Blueprint graphs to walk; use `blueprintPath` for
    the per-RPC / per-property detail.

**Not part of the read-back on either branch** (write-only setter — don't try
to confirm it via `get_networking_info`): the **replicate-movement** flag that
`configure_replicated_movement` sets (`bReplicateMovement` is emitted by neither
branch). Its absence from the payload is the documented contract, not a call
error — verify that state by other means.
