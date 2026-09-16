# bpir.types

BPIR type system, literal formats, FText forms, optional pin handling, and type-string resolution. See `call("bpir")` for the language overview.

## 3. Type System

```
bool, byte, int, int64, float, double, string, text, name
object<ClassName>           # UObject reference
class<ClassName>            # Class reference
struct<StructName>          # Struct value
enum<EnumName>              # Enum value
array<Type>                 # Array
set<Type>                   # Set
map<KeyType, ValueType>     # Map
softobject<ClassName>       # Soft reference
softclass<ClassName>        # Soft class reference
interface<InterfaceName>    # Interface reference
delegate<SignatureName>     # Single-cast delegate
mcdelegate<SignatureName>   # Multi-cast delegate (event dispatcher)
delegate<OwnerHint, SignatureName>    # Single-cast delegate with signature-owner hint
mcdelegate<OwnerHint, SignatureName>  # Multi-cast delegate with signature-owner hint
wildcard                    # Wildcard (generic)
```

Delegate typed forms use the comma form `delegate<OwnerHint, SignatureName>` and `mcdelegate<OwnerHint, SignatureName>` when the signature owner matters. `OwnerHint` is only a lookup hint for the signature owner; compilation must resolve `SignatureName` to the real `UFunction` signature, including package-owned native delegate signatures, and write that function into `PinSubCategoryMemberReference`. Decompilation emits a parser-safe class, module, or package owner hint when the member reference is populated so the text can compile back to the same delegate signature.

**Note:** `set<T>` and `map<K, V>` are fully supported on both the compile and
decompile paths as of the structured type-spec parser refactor. Earlier
versions only produced them from the decompiler; parsing them failed silently.

**Struct names — bare vs. tagged.** Any `UScriptStruct` type is accepted bare
by name; `struct<TypeName>` is an optional explicit form. The well-known
engine structs — `Vector`, `Rotator`, `Transform`, `LinearColor`, `Color`,
`IntPoint`, `IntVector`, `Quat`, `Plane`, `Box` (and their `F`-prefixed
aliases like `FVector`, case-insensitive) — take a parse-time fast path
through the grammar table. Any other struct name (e.g. `FMyGameStruct`,
`FEditorReplay`) resolves through the generic `UEnum → UScriptStruct →
UClass` cascade inside `ConvertTypeSpecToPinType` and works identically.

Use `struct<TypeName>` when you want to:

- Disambiguate on name collisions. If the same identifier exists as both a
  `UEnum` and a `UScriptStruct`, the cascade resolves the enum first. The
  tagged form forces the struct lookup.
- Signal intent explicitly in source — a reader sees `struct<Foo>` and
  knows it's a struct without having to check what `Foo` is.
- Match decompiler output. The decompiler always emits `struct<Name>` for
  every struct pin regardless of input alias, so `parse → decompile`
  round-trips normalise to the tagged form.

## Literal Formats

```
true, false                                     # bool
42, -7, 0xFF                                    # int
3.14, -0.5, 1e3                                 # float
"Hello World"                                   # string (backslash escaping)
nullptr                                         # null object
EMyEnum::ValueName                              # enum
FVector(1.0, 2.0, 3.0)                         # vector
FRotator(0.0, 90.0, 0.0)                       # rotator
FLinearColor(1.0, 0.0, 0.0, 1.0)              # color
/Game/Path/To/Asset.Asset                       # asset reference
```

## FText Literal Forms

FText pin defaults accept three quoted-string forms, all routed through
`FTextStringHelper::CreateFromBuffer` (the same engine path UE uses for
`FText::ToString` / asset serialization):

```
"Plain display"                                 # invariant FText (no localization identity)
"NSLOCTEXT(\"Namespace\", \"Key\", \"Display\")"  # localized FText with namespace+key
"LOCTABLE(\"TableId\", \"Key\")"                  # FText backed by a UStringTable asset
```

- **Plain string** — produces an invariant (culture-invariant) FText. Use only
  when no localization identity exists; pin acceptance still requires either an
  existing identity to inherit or one of the wrapped forms below for fresh pins.
- **NSLOCTEXT** — gives the FText a namespace+key identity that the localization
  gather pipeline picks up at cook time. Round-trips through decompile/recompile.
- **LOCTABLE** — references an entry in a `UStringTable` asset. Resolves at
  runtime via `FText::FromStringTable(TableId, Key)`. The referenced table asset
  must be loadable at runtime; the parse succeeds even if the table isn't loaded
  yet at compile time. Round-trips through decompile/recompile preserving the
  table linkage.

## Optional Empty Pins

The decompiler omits unwired optional empty pins instead of emitting placeholder values. This includes loaded `FKey` inputs whose pins are `PC_Struct` with an empty default, even when the loaded pin no longer has `PinSubCategoryObject`; the omitted `PC_Name` / `OptionalName` case remains a separate optional-name rule.

Implementation detail: do not classify `PC_Struct` optional call pins from `UEdGraphPin::PinType.PinSubCategoryObject` alone. Loaded Blueprint call pins can lose that object pointer while still carrying a default-equivalent text value such as `None`. For `UK2Node_CallFunction` pins, recover the `UScriptStruct` from the target `UFunction` parameter, import the pin text into an `FStructOnScope`, and compare it with a default instance through `UScriptStruct::CompareScriptStruct` before deciding whether the argument is omittable.

## Type-string resolution

As of the structured type-spec parser refactor, every type-string
resolution — BPIR entry signatures, body pin types, and the JSON-layer handlers
that synthesize pin types from arbitrary user-supplied strings — funnels
through a single pipeline:

1. `BpirTypeSpecParser::ParseTypeSpec` tokenizes/parses the string into a
   structured `FBpirTypeSpec` using the `BpirTypeGrammar` table (the single
   source of truth for which bare identifiers map to which primitive kinds and
   which well-known struct types).
2. `FCodePinResolver::ConvertTypeSpecToPinType` converts the spec to an
   `FEdGraphPinType`, setting any `bIsConst` / `bIsReference` flags carried
   on the spec.

The legacy string-only resolver `FCodePinResolver::ConvertCppTypeToPinType(FString)`
and its helpers (`BlueprintHandlerUtils::MakePinType`, `ParseAngleBracketType`,
`NormalizeParamType`) were removed in Phase 5. Callers that previously handed
a raw string to those helpers now parse via `BpirTypeSpecParser::ParseTypeSpec`
and convert via `ConvertTypeSpecToPinType` — there is a single entry point and
a single grammar definition. `BuildNamedPinDescriptor` accepts an
`FBpirTypeSpec` directly; JSON handlers parse the user-supplied type string
themselves and surface `FormatTypeSpecErrorDetail` in error messages on parse
failure.
