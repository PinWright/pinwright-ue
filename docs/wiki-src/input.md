# input

Authors Enhanced Input assets — `UInputAction` (IA) assets describe an abstract action (Jump, Fire, Look), `UInputMappingContext` (IMC) assets bind hardware keys to those actions at a priority. This namespace only authors the assets; runtime activation via `AddMappingContext` on the Enhanced Input local-player subsystem happens in gameplay BPs / C++ — use `call("ui")` for the widget side of an input loop and `call("editor.console_command")` to issue runtime input via console.

## Cross-cluster overlap

A typical setup creates one IA per action and one IMC per gameplay mode, then calls `input.add_mapping` once per `(IMC, IA, key)` triple. `input.get_input_info` verifies an IA's value type / consume flag or reads an IMC's complete default key mappings without opening the editor.

**Authoring triggers and modifiers**

Triggers and modifiers exist at two different levels. Use `property.set` for an IA's action-level `Triggers` / `Modifiers` arrays. For one IMC key mapping, pass `modifiers` / `triggers` to `input.add_mapping`; those objects run before the action-level objects.

On UE 5.7 and newer, including UE 5.8, the IMC's live default array is `DefaultKeyMappings.Mappings` (`default_key_mappings.mappings` in Python). The legacy top-level `Mappings` property is deprecated and can read back empty. UE 5.3–5.6 store the same data in the legacy `Mappings` array. Prefer `input.add_mapping` and `input.get_input_info` over reflected writes so the plugin follows the correct store for the running engine version.

Runtime context activation still happens in gameplay code through `AddMappingContext` on the Enhanced Input local-player subsystem, not here.

### input.create_input_action

Creates a `UInputAction` (IA). It takes only `name` + `path`; `valueType` is **not** a
create parameter (`[UNKNOWN_PARAMS]`). Every new IA is **Boolean**
(`EInputActionValueType::Boolean`), the engine default.

For **Axis1D / Axis2D / Axis3D**, create the IA, then set its reflected `ValueType`
with `property.set`:

```
call("input.create_input_action", { "name": "IA_Move", "path": "/Game/Input" })
call("property.set", {
  "objectPath": "/Game/Input/IA_Move.IA_Move",
  "propertyName": "ValueType",
  "value": "Axis2D"
})
```

`value` accepts `Boolean`, `Axis1D`, `Axis2D`, or `Axis3D` (also the underlying int).
`input.get_input_info` reports the new `valueType` (`Axis2D` reads back as `"2"`).
Use `property.set` for the IA's action-level `Triggers` and `Modifiers`; use
`input.add_mapping` for per-key objects on an IMC mapping.

### input.add_mapping

Bind one key to one Input Action in an IMC. Call repeatedly for multiple keys (for example,
W and Up-Arrow → MoveForward). Each call loads, mutates, and saves the IMC; for many bindings,
batch calls while the editor is closed for that asset to avoid auto-reload churn.

Example:

```
call("input.add_mapping", {
  "contextPath": "/Game/Input/IMC_Default.IMC_Default",
  "actionPath": "/Game/Input/IA_Move.IA_Move",
  "key": "W",
  "modifiers": [
    {
      "class": "InputModifierSwizzleAxis",
      "properties": { "Order": "YXZ" }
    }
  ],
  "triggers": []
})
```

Each modifier or trigger entry is `{class, properties}`. `class` accepts a short
Enhanced Input class name or a full class path; `properties` is optional and uses the
same reflected-property conversion as `property.set`. Entries are stored in array order,
and each created object is owned by the IMC so it is saved with the mapping. The response
echoes the stored `key`, `modifiers`, and `triggers`, not the unverified request values.

Hardware key strings come from UE's `EKeys` table (`SpaceBar`, `LeftShift`, `Gamepad_FaceButton_Bottom`, `MouseScrollUp`, …).

### input.get_input_info

For an IMC, the result includes `mappingCount`, `mappingStorage`, and `mappings`. Each
mapping contains its `actionPath`, `key`, and ordered `modifiers` / `triggers`; each
instanced object includes `class`, `classPath`, and its editable `properties`. On UE 5.7+
`mappingStorage` is `DefaultKeyMappings.Mappings`; on UE 5.3–5.6 it is `Mappings`.
