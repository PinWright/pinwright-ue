PinWright generates and refreshes its on-disk wiki when the Unreal Editor starts. Read `{{PINWRIGHT_WIKI_DIRECTORY}}/index.md`, then use filesystem search and file-reading tools in `{{PINWRIGHT_WIKI_DIRECTORY}}/` as the default discovery workflow.

The wiki is flat:

- `index.md` is the root page.
- `<namespace>.md` documents a namespace.
- `<namespace.method>.md` documents a method.

Invocation modes:

- `call({method: "<namespace-or-method>"})` without `args` returns documentation, but direct filesystem search and file reading in the generated wiki are preferred over fetching documentation through MCP.
- `call({method: "<namespace.method>", args: {...}})` executes the RPC. Methods with no parameters still require `args: {}` to execute.

Read the exact method page before execution.
