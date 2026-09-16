# data_table

Author and inspect `UDataTable` rows — list rows (row struct + current contents), add/set/remove rows, and set rows from a typed struct payload. Use `call("property.set")` against the asset for table-level UPROPERTYs that aren't row data; this namespace is scoped to row-content CRUD.

`data_table.add_row`, `data_table.set_row`, `data_table.remove_row`, and `data_table.set_row_struct` persist by default. Pass `save:false` to keep a mutation in memory for a later explicit save. Every successful mutation reports `saveRequested`, `saved`, `saveState`, and `saveDetail`; `pendingFlush:true` appears only when a requested save did not become durable. Read `saveState` for the remedy rather than treating the mutation's successful in-memory result as proof that the `.uasset` reached disk.
