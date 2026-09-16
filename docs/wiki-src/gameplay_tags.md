# gameplay_tags

Authoring surface for the project's `GameplayTagsManager` registry and `FGameplayTagQuery` values on configured assets. Registry mutation (add / remove / list / add_source) is INI-backed through `IGameplayTagsEditorModule`; `build_query` walks a recursive JSON expression tree, serializes an `FGameplayTagQuery`, and can write it to a target asset property in one call. Use this namespace for registry work and declarative query authoring; use `call("gas")` for GAS asset bodies after registration or `call("property")` for other property writes.

## Cross-cluster overlap

- **Registry vs application** — `gameplay_tags.add` declares a tag exists; it does not assign the tag to any asset. Per-asset application lives in [`gas`](gas.md) (effects, abilities, attribute sets) or via [`property`](property.md) for arbitrary `FGameplayTagContainer` properties.
- **Source-scoped adds** — `gameplay_tags.add(tag, sourceB)` must not globally short-circuit when the same tag already exists in `sourceA`; the source entry needs to actually exist. This is a known IN-REVIEW caveat for the namespace.

## See also

- [`gas`](gas.md) for applying tags to GameplayAbility / GameplayEffect / AttributeSet assets.
- [`property`](property.md) for arbitrary `FGameplayTagContainer` / `FGameplayTagQuery` writes when `build_query`'s target path is not enough.
- [`asset`](asset.md) for `properties.json` dumps of tag and query values.

### gameplay_tags.list

Lists registered tags with `name` / `source` / `comment` / `isExplicit`, plus `sourceType` and the full `configFile` path. The un-prefixed overview returns **all** tags; on a normal project (hundreds of tags), those paths can exceed the 10000-character inline display budget, so the result spills to `Saved/PinWright/HttpResponses/...` for `Read`/`Grep`. To keep it inline:

- **`prefix` / `source`** — narrow to a tag subtree (`prefix:"Ability.Combat"`) or one INI source. This is the hardest narrowing and returns inline whenever the subtree is small.
- **`limit`** — caps the returned rows (clamped to `[1, 500]`, **default 50**). `totalMatches` always reports the full untruncated match count and `truncated` flips true when the cap elided rows, so elision stays detectable; pass `includeTotal:true` to keep scanning past the cap for an exact `totalMatches`.
- **`namesOnly:true`** (alias `names_only`) — drop the byte-dominating per-row `configFile` path plus `sourceType` and `comment`, returning only `name` / `source` / `isExplicit`. This alone collapses the payload for the common "just show me the tag names" read.
- **`fields`** — a per-row allow-list (array or bare string) of `name` / `source` / `comment` / `isExplicit` / `sourceType` / `configFile` for finer control (e.g. `fields:["name"]`).

### gameplay_tags.build_query

A single call replaces `property.get` + manual serialization + `property.set`.

**Expression tree.** Each node carries an `op` plus either a `tags` array (leaf ops) or an `expressions` array (composite ops). Mixing both on one node fails with `MIXED_PAYLOAD`.

| `op` value (snake_case or camelCase) | Engine method | Kind |
|---|---|---|
| `any_tags_match` / `anyTagsMatch` | `FGameplayTagQueryExpression::AnyTagsMatch()` | leaf |
| `all_tags_match` / `allTagsMatch` | `AllTagsMatch()` | leaf |
| `no_tags_match` / `noTagsMatch` | `NoTagsMatch()` | leaf |
| `any_expressions_match` / `anyExpressionsMatch` | `AnyExprMatch()` | composite |
| `all_expressions_match` / `allExpressionsMatch` | `AllExprMatch()` | composite |
| `no_expressions_match` / `noExpressionsMatch` | `NoExprMatch()` | composite |

The engine's composite-op fluent builders are abbreviated (`AnyExprMatch`, not `AnyExpressionsMatch`); the JSON `op` accepts the long form for readability and the handler maps it through. Leaf ops use the unabbreviated `*TagsMatch` form on both sides. Construction goes through `FGameplayTagQuery::Build(Expression, Description)`; the `QueryTokenStream` byte buffer is private with no accessor, so the response reports a size proxy — `tokenStreamBytes` is the built query serialized through `FMemoryWriter` (`Buffer.Num()`).

**Target writes resolve Blueprint paths to the CDO.** When `target.assetPath` resolves to a `UBlueprint`, the handler walks `BP->GeneratedClass->GetDefaultObject()` and writes onto the CDO rather than mutating `UBlueprint` internals, surfacing the redirection via `resolvedTargetPath` so callers can confirm where the write landed. Both `target.assetPath` and `target.propertyPath` are required when `target` is present.

**Result shape.**

```json
{
  "description": "...",
  "tokenStreamBytes": 36,
  "wrote": true,
  "resolvedTargetPath": "/Game/UI/W_Foo.W_Foo_C"
}
```

`resolvedTargetPath` is only present when the asset path resolved through a Blueprint to its CDO; bare struct asset writes omit the field.

**Error codes.** `UNKNOWN_OP` (op string matched no leaf or composite), `MIXED_PAYLOAD` (leaf op carried `expressions`, or composite op carried `tags`), `EMPTY_TAGS` / `EMPTY_EXPRESSIONS` (required array missing or empty), `UNKNOWN_TAG` (tag entry didn't resolve through `UGameplayTagsManager::RequestGameplayTag`), `MAX_DEPTH_EXCEEDED` (recursion exceeded `maxDepth`), and `ASSET_NOT_FOUND` / `PROPERTY_NOT_FOUND` / `PROPERTY_WRONG_TYPE` (target write failed before mutation).

**Example** — apply an "any of these tags, but not these other tags" query into a Blueprint CDO field:

```json
{
  "method": "gameplay_tags.build_query",
  "args": {
    "expression": {
      "op": "all_expressions_match",
      "expressions": [
        { "op": "any_tags_match", "tags": ["Ability.Attack.Melee", "Ability.Attack.Ranged"] },
        { "op": "no_tags_match",  "tags": ["State.Cooldown"] }
      ]
    },
    "description": "MeleeOrRanged.NotOnCooldown",
    "target": {
      "assetPath": "/Game/Abilities/BP_BaseAbility",
      "propertyPath": "ActivationRequiredTagsQuery"
    }
  }
}
```
