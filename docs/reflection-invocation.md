---
type: reference
summary: "Rules for invoking a UFunction via UObject::ProcessEvent with a caller-built parameter buffer — FProperty init/destroy lifecycle, aligned Function->ParmsSize storage, CPF_OutParm/CPF_ConstParm classification, raw-container property export, CDO refusal, and AssetUtils::ResolveUObjectByPath for any-UObject targets. Canonical implementation: object.call_function (ObjectCallFunctionHandler.cpp)."
date: 2026-09-04
tags: [reflection, ufunction-invocation, fproperty-lifecycle, process-event, object-call-function, property-utils, generic-primitives]
---

# Reflection-Based UFunction Invocation

Engineer-facing reference for anyone adding a reflection-based RPC handler that
needs to call a `UFunction` on a runtime `UObject` (actor / component / asset /
subsystem / transient) and ferry JSON arguments + return values + out params.

The canonical implementation is `object.call_function`
(`Private/Handlers/Reflection/ObjectCallFunctionHandler.cpp`). Prefer routing
new callers through it; the older `actor.call_function` (since removed) was
parameterless-only back-compat — it zero-inited the parm buffer, ignored
return values, and only accepted actor targets. Do **not** copy its pattern
for new work.

## 1. Parameter buffer lifecycle: init before write, destroy after read

`UObject::ProcessEvent(Fn, Parms)` expects a packed memory buffer of
`Fn->ParmsSize` bytes laid out by the function's parm chain
(`TFieldIterator<FProperty>` filtered by `CPF_Parm`). The buffer is the
caller's responsibility to construct and tear down, and `FMemory::Memzero`
alone is **not** a valid construction for non-trivial property types.

Allocate the buffer with the function's own size and alignment:
`FMemory::Malloc(Function->ParmsSize, Function->GetMinAlignment())`. A
hand-built `TArray<uint8>` can appear to work for trivial signatures but
misalign struct parameters and returns. The visible symptom is especially
misleading for structs such as `FVector`: `ProcessEvent` returns, the RPC
reports success, but input params are ignored or return/out params export as
zero because the property machinery read the wrong slot.

Only initialize properties whose flags include `CPF_Parm`. Locals and other
reflected properties on the owning class are not part of the parameter frame,
even if they appear in a broader field iterator.

For every `FProperty` in the parm chain you must call
`Property->InitializeValue_InContainer(Parms)` before writing any
JSON-derived value into the buffer slot. Zero-init is sufficient only for
trivially-constructible types (raw scalars: int/float/bool slots). It is
undefined for:

- `FString` — owns an allocator slot; zero-init produces a string whose
  destructor will read garbage.
- `FText` — refcounted pointer to a localized text instance.
- `FName` — packed index into the global name table; index 0 collides with
  `NAME_None` in some paths but the property machinery still expects an
  initialized slot.
- `FArrayProperty` / `FMapProperty` / `FSetProperty` — allocator + slack
  accounting in the container header.
- Any `USTRUCT` with a non-trivial constructor — `FInstancedStruct`,
  `FGameplayTag`, `FInstancedPropertyBag`, and any struct that doesn't opt
  into `WithNoInitConstructor`.

Symmetrically, after `ProcessEvent` returns and you have exported return /
out params to JSON, walk every property again and call
`Property->DestroyValue_InContainer(Parms)` before freeing the buffer.
Skipping the destroy walk leaks the FString / FArray / FStruct allocations
on every call.

The destroy walk must run on **every exit path** — both the success path
and every early-error path between init and `ProcessEvent`. The robust
pattern is: as you initialize each property, record it in a local
`TArray<FProperty*> InitializedProps`; on the way out (success or error),
iterate `InitializedProps` and destroy exactly those slots. Do not iterate
the `UFunction`'s `TFieldIterator` for cleanup if there is any chance an
early return short-circuited the init loop — that would destroy
uninitialized slots.

## 2. Out-param classification

The parm-flag combination on each `FProperty` decides whether a slot is an
input, an output, or both. The canonical four-way split:

| Flags                                                            | Direction       | Read from args? | Export in outParams? |
|------------------------------------------------------------------|-----------------|-----------------|----------------------|
| `CPF_ReturnParm`                                                 | return slot     | no              | as `returnValue`     |
| `CPF_OutParm \| CPF_ConstParm` (no `CPF_ReturnParm`)             | const-ref input | yes             | no                   |
| `CPF_OutParm` only (no `CPF_ConstParm`, no `CPF_ReturnParm`)     | real out-param  | no              | yes                  |
| `CPF_OutParm \| CPF_ReferenceParm` (no `CPF_ConstParm`)          | in-out          | yes             | yes                  |
| (default — none of the above)                                    | pure input      | yes             | no                   |

The `CPF_ConstParm` bit is the key disambiguator. Many engine UFunctions
declare parameters as `const FFoo& Bar`, which the reflection system
surfaces as `CPF_OutParm | CPF_ConstParm` — it looks like an output but is
really an input. A common bug is to treat anything with `CPF_OutParm` set
as an output, skipping the args lookup and exporting an uninitialized
slot. Always test `CPF_ConstParm` before deciding direction.

## 3. CDO refusal

`ProcessEvent` on a target with `RF_ClassDefaultObject` is broadly unsafe.
The CDO has no world, no live subobjects, and no real component graph;
functions that touch `GetWorld()`, owned components, or actor state will
crash or silently do nothing. Reject CDO targets at the resolve step with
an `INVALID_TARGET` error and require the caller to pass a real instance
path.

## 4. Resolving "any UObject" by path

Use `AssetUtils::ResolveUObjectByPath(Path, OutError)`. It tries
`StaticFindObject(UObject::StaticClass(), nullptr, *Path, false)` first,
then falls back to `StaticLoadObject(UObject::StaticClass(), nullptr, *Path)`
on miss.

- `StaticFindObject` catches in-memory transient instances, subsystem
  objects, components addressed via the `Actor.Component` two-segment form,
  and already-loaded assets.
- `StaticLoadObject` is the cold-load fallback for assets that aren't yet
  resident.
- Do **not** use the template `LoadObject<UObject>` — its template
  signature wants a concrete class and won't compile against
  `UObject::StaticClass()` cleanly.

## 5. `ParmsSize == 0` short-path

Parameterless UFunctions skip the buffer entirely — call
`Target->ProcessEvent(Fn, nullptr)`. The init / destroy / classification
machinery only matters when `Fn->ParmsSize > 0`. Branching on this up
front keeps the simple case simple, the same short-path the since-removed
`actor.call_function` took.

## 6. Transaction wrap for mutating calls

For mutating UFunction invocations, wrap the call in `FScopedTransaction`
and call `Target->Modify()` before `ProcessEvent` so undo works on actor
and asset writes. The wrap is harmless on transient or non-transactional
objects, so it's safe to apply uniformly when the caller asks for a
write-style invocation.

## 7. JSON ↔ FProperty layer

The reusable property-marshalling layer is `PropertyUtils`:

- `PropertyUtils::ApplyJsonValueToProperty(Container, Property, JsonValue, OutError)`
  — writes a `TSharedPtr<FJsonValue>` into an FProperty slot.
- `PropertyUtils::ExportPropertyToJsonValue(Source, Property)` — reads an
  FProperty slot back out as a JSON value. `Source` records raw storage versus
  explicit UObject ownership.

Both handle the common scalar set (bool, str, name, text, numeric,
byte+enum), object types (object + soft-object, class + soft-class),
structs, and containers (array, set, map). Reuse these instead of
hand-rolling JSON coercion in the call-function handler.

Property storage and ownership are separate contracts. A `ProcessEvent`
parameter frame or independently supplied USTRUCT address uses
`FPropertyExportSource::FromRaw`. Its nested optional and collection addresses
remain raw. Nested storage reached from `FromObject`, however, retains that
verified owner provenance so `UPROPERTY(Instanced)` array and map elements keep
their authored expansion without treating the element address as a `UObject`.
Every path derives the property value address with
`FProperty::ContainerPtrToValuePtr` and reads hard object references through
`FObjectPropertyBase::GetObjectPropertyValue`; it must never cast the
container address to `UObject*`. Hard object references from a raw source always
remain path/null even when the property carries instancing flags. Soft-object
properties retain their soft path without forcing a load.

Typed `UObject*` pointers select the owned source automatically. When a known
owner is held as `void*` after nested-path resolution, pass
`FPropertyExportSource::FromResolvedContainer(Container, ResolvedOwner)` so the
shared factory selects `FromObject` only when both addresses are identical and
otherwise keeps the source raw. Explicit ownership is the only path allowed to
expand an instanced object or owned component, or resolve a Blueprint SCS
component template from a CDO. External components remain path-shaped. The
encapsulated source-wrapper API also makes `nullptr` unambiguous.

Reflection RPC outputs use `ExportPropertyToJsonValueStrict` for every return
and out slot. If any output contains an unsupported property shape,
`object.call_function` destroys the initialized parameter values and returns
`UNSUPPORTED_PARAM_TYPE` for the whole request. Dump-style callers keep the
legacy `{ "_kind": "unsupported" }` marker so inspection remains loss-tolerant.
Strict support status is propagated during recursive export; it is not inferred
by scanning emitted JSON, so valid user data with `_kind` and `cpp_type` keys is
not mistaken for the legacy marker. Owned subobjects, structs, instanced structs,
optionals, arrays, maps, and sets all pass the same result state upward and stop
at the first unsupported child. Only the outer loss-tolerant adapter creates the
legacy marker. Map keys retain Unreal's canonical text form (including exact
FGuid and integer text) after their supported key shape is checked.

## See also

- [arch.md](arch.md) — overall handler architecture, auto-registration,
  FHandlerContext, and where this primitive sits in the dispatcher flow.
- `Private/Handlers/Reflection/ObjectCallFunctionHandler.cpp` — canonical
  implementation of all seven rules above.
- `Private/Handlers/Actor/ActorPropertyHandler.cpp` — former home of the
  legacy parameterless-only `actor.call_function` predecessor (since removed).
- `Private/Utils/PropertyUtils.cpp` — JSON↔FProperty conversion layer
  used by step 7.
- [wiki-src/property.md](wiki-src/property.md) — caller-facing reflection property
  accessors that share the same JSON↔FProperty conversion layer.
- `Private/Utils/AssetUtils.cpp` (`ResolveUObjectByPath`) — generic
  UObject-by-path resolver used by step 4.
- Board ticket `F-object-call-function` — feature ticket and design
  history for this primitive.
- Agent-facing wiki overlay lives in `docs/wiki-src/object.md` if/when one is
  authored; this page is the engineer-facing counterpart.
