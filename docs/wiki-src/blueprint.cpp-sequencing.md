# Sequencing C++ edits with BPIR patches

## Sequencing C++ Edits With BPIR Patches

When a fix spans both C++ and a Blueprint (e.g. a new `UFUNCTION()` plus a BP graph that calls it), **do not patch the BP via MCP before the editor has hot-loaded the new C++ symbol**. The running editor's reflection database only knows the UFunctions present in its currently-loaded module DLLs. If `compile_bpir` references a UFunction that exists in source but not in the live editor:

- The BPIR compiler's function resolver falls back to `FindFirstObjectSafe` / iterator search and returns null.
- Some handlers will emit a `K2Node_CallFunction` with a stale `FunctionReference` that compiles in BPIR but fails at Kismet compile time with `"Unable to find the selected function/event"`.
- Worst case: the asset gets saved with a dangling function reference, producing an un-loadable `.uasset` next launch.

**Workflow:**

1. Apply the C++ change in source.
2. Ask the user to recompile (Live Coding or full Build → `<Project>Editor`).
3. Wait for explicit confirmation that the editor has hot-loaded.
4. Optionally verify the symbol is live: `call("blueprint.build_api_index", { classFilter: ["<ClassNameWithoutPrefix>"] })` then `call("blueprint.search_api", { query: "NewFunctionName" })`, or `call("blueprint.inspect", ...)` for a Blueprint-defined function.
5. Only then run `compile_bpir` / `insert_bpir_at_node`.

This is a corollary of the broader MCP BP corruption rules — never save a BP that references symbols the live editor doesn't know about.

## See also

- [`blueprint`](blueprint.md) — the compile/decompile/inspect verbs this sequence drives.
- [`bpir`](bpir.md) — the BPIR language the patch is written in.
- [`blueprint.bpir-gotchas`](blueprint.bpir-gotchas.md) — the wider corruption rules this is a corollary of.
- [`system`](system.md) — `system.live_coding_status` / `system.live_coding_compile` for confirming the editor hot-loaded the new symbol.
