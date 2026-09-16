# chooser

Author `UChooserTable` assets — create the table, add columns and rows, set cell predicates, set per-row result objects, and compile the chooser graph. Use this for chooser-table authoring; for runtime evaluation or reading back compiled state, use `call("property.get")` against the asset path.

## Availability

`chooser.*` requires the **Chooser** engine plugin. The integration auto-loads when that plugin is enabled in the host project; when it is disabled, these methods are unregistered and calling one returns `PLUGIN_DISABLED` (enable the Chooser plugin and restart the editor).

## The column → context → propertyBinding contract

A chooser's **input** columns (`bool`, `float`, `enum`, `object`) do not store a value to compare — they read a property off the table's **context object** at evaluation time. For such a column to bind (and to compile without errors) two things must both be in place:

1. The table must declare a context object class — pass `contextObjectType` to `chooser.create`. This is **effectively mandatory** for any chooser with property-driven columns, not an optional convenience: with no context class there is nothing for a column to read against.
2. Each input column must carry a `propertyBinding` — a property path (e.g. `MinDrawDistance`, `bVisible`, or a dotted chain) resolving a property on that context class. Pass it to `chooser.add_column`.

`chooser.create` and `chooser.add_column` **succeed silently** when these are omitted — the asset is created and the column is added — but the gap is not reported until `chooser.compile`, which emits one `No Property Bound` error diagnostic per unbound column **after the whole table is already built**. There is no `set_column_binding` / `set_context` retrofit verb, so recovering an unbound table means deleting it and rebuilding from scratch. To avoid that, `chooser.add_column` now returns a non-fatal `warnings` entry the moment a property-driven column is added without a resolvable binding (no context class on the table, or no `propertyBinding` on the column).

Note: semantic inputs like "viewer distance" or "is hero" have no stock-class property — you must map them onto whatever property the chosen context class actually exposes (e.g. `StaticMeshComponent.MinDrawDistance` for distance, `bVisible` for a hero flag). The `randomize` column kind has no input property and needs no context or binding.

## Build order

Author choosers in this order; adding input columns *after* rows is not the documented order:

1. `chooser.create` with `contextObjectType` set to the context class.
2. `chooser.add_column` once per input column, each with its `propertyBinding` (plus `enumType` / `allowedClass` for enum/object kinds).
3. `chooser.add_row` for each rule row.
4. `chooser.set_cell` to set each column's per-row predicate.
5. `chooser.set_result` to set each row's result object/class.
6. `chooser.compile` — expect `diagnostics: []` when every input column is bound.

## See also

- [property](property.md) for `property.get` read-back against the compiled chooser asset path.

### chooser.create

Create a `UChooserTable` asset. Pass `contextObjectType` (a UObject class path such as `/Script/Engine.StaticMeshComponent`) whenever the chooser will have property-driven input columns — which is essentially any chooser with `bool` / `float` / `enum` / `object` columns. Although the parameter is technically optional, omitting it leaves the table with no context object for columns to read against, so every input column will fail `chooser.compile` with `No Property Bound`. Only a chooser built entirely from `randomize` columns can safely omit it.

`resultType` selects what each row resolves to (`object`, `class`, or `none`); `outputObjectType` constrains an `object`-result chooser's output class.

**`name` is a bare asset name, not a path.** Supply `path` alone and it is the whole package path for the new table. Supply `name` as well and `path` becomes the destination FOLDER while `name` is the leaf — and the leaf is validated as a leaf: a `name` containing `/`, `\`, `.`, `:` or a leading/trailing slash is refused `INVALID_ARGUMENT` quoting the engine's own reason, rather than being concatenated onto the folder. That concatenation used to write the table to a package the caller never named (`name: "Sub/Leaf"`), and a `name` containing `//` reached `CreatePackage`, which logs it at Fatal and ends the editor process. Nest the destination by naming it in `path` and omitting `name`.

### chooser.add_column

Append a first-slice column. For property-driven kinds (`bool`, `float`, `enum`, `object`) pass `propertyBinding` — a property path on the table's context class — or the column will report `No Property Bound` at `chooser.compile`. When a property-driven column is added without a resolvable binding (the table has no context class, or no `propertyBinding` is supplied), the response includes a non-fatal `warnings` array spelling out what is missing; the column is still added, so this is a heads-up, not an error.

`enum` columns also take `enumType`; `object` columns also take `allowedClass`. The `randomize` kind needs no binding (it has no input property) and emits no warning.

### chooser.set_cell

Set one column's per-row predicate. The single `value` param is declared as `"any"` because its accepted JSON shape is **dispatched by the stored column kind** (fixed by `chooser.add_column`), so the same `value` field means four different things:

| column kind | accepted `value` shape |
| --- | --- |
| `float` | object `{min, max}` for a range (omit either bound for an open-ended range), or a bare number for a point match (`min == max`) |
| `bool` | a JSON bool (`true`/`false`), or a string token `"true"`/`"false"`/`"any"` (also `"match_true"`/`"match_false"`/`"match_any"`); default is `MatchAny` |
| `object` | object `{value: "<asset/object path>", comparison: "equal"\|"not_equal"\|"any"}` (the path key also accepts the alias `assetPath`), or a bare path string (defaults to `equal`) |
| `enum` | object `{value: "<enum value name>"\|<number>, comparison: "equal"\|"not_equal"\|"any"}` — the column's `enumType` from `add_column` resolves the value name |
| `randomize` | a bare number (the row weight) |

The column kind is authoritative: a `{min,max}` object passed to a `bool` column, or a bare bool passed to a `float` column, will not be reinterpreted — match the shape to the kind you added.

### chooser.compile

Compile the table and return `diagnostics`. Each unbound input column produces a `kind: "column"` diagnostic with the engine's `No Property Bound` message and its `columnIndex`; each unresolved result produces a `kind: "result"` diagnostic with its `row`. A clean build returns `diagnostics: []` and `hasDiagnostics: false`. If you see `No Property Bound`, the fix is to set `contextObjectType` on `chooser.create` and a `propertyBinding` on each affected column — there is no in-place binding-repair verb, so an already-built unbound table must be deleted and rebuilt in the documented order above.
